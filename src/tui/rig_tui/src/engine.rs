use std::ffi::{CStr, CString, c_char};
use std::ptr;

use rig_ui::model::{
    Device, Engine, EngineError, HIST_BUCKETS, Histogram, Param, Snapshot, Strip, Taper,
};

use crate::ffi::{
    self, RtDeviceDesc, RtOpenConfig, RtParamDesc, RtSession, RtSnapshot, RtStripDesc,
};

const _: () = assert!(HIST_BUCKETS == ffi::RT_HIST_BUCKETS);

pub struct OpenOptions {
    pub backend: Option<String>,
    pub input: Option<String>,
    pub output: Option<String>,
    pub rate: f64,
    pub block: i32,
    pub exclusive: bool,
}

pub struct FfiEngine {
    s: *mut RtSession,
    strips: Vec<Strip>,
    device: Device,
}

fn from_c_array(arr: &[c_char]) -> String {
    let bytes: Vec<u8> = arr
        .iter()
        .take_while(|&&c| c != 0)
        .map(|&c| c as u8)
        .collect();
    String::from_utf8_lossy(&bytes).into_owned()
}

fn from_c_ptr(p: *const c_char) -> String {
    if p.is_null() {
        return String::new();
    }
    // SAFETY: rt_ffi returns pointers to NUL-terminated string literals that live
    // for the whole process.
    unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
}

fn c_string(s: &Option<String>) -> Result<Option<CString>, EngineError> {
    s.as_deref()
        .map(CString::new)
        .transpose()
        .map_err(|_| EngineError::Arg("argument contains a NUL byte".into()))
}

fn index(i: usize) -> Result<i32, EngineError> {
    i32::try_from(i).map_err(|_| EngineError::Arg("index out of range".into()))
}

impl FfiEngine {
    pub fn open(o: &OpenOptions) -> Result<Self, EngineError> {
        // SAFETY: plain constructor; null is checked below.
        let s = unsafe { ffi::rt_session_create() };
        if s.is_null() {
            return Err(EngineError::Internal("could not create a session".into()));
        }
        let mut e = FfiEngine {
            s,
            strips: Vec::new(),
            device: Device::default(),
        };

        let backend = c_string(&o.backend)?;
        let input = c_string(&o.input)?;
        let output = c_string(&o.output)?;
        let ptr_of = |c: &Option<CString>| c.as_ref().map_or(ptr::null(), |c| c.as_ptr());
        let cfg = RtOpenConfig {
            backend: ptr_of(&backend),
            input_id: ptr_of(&input),
            output_id: ptr_of(&output),
            sample_rate: o.rate,
            block_frames: o.block,
            exclusive: u8::from(o.exclusive),
        };
        // SAFETY: `e.s` is live; `cfg` and the CStrings it points to outlive the call.
        e.check(unsafe { ffi::rt_session_open(e.s, &cfg) })?;
        e.device = e.read_device()?;
        e.strips = e.read_strips()?;
        Ok(e)
    }

    fn last_error(&self) -> String {
        let mut buf: [c_char; 512] = [0; 512];
        // SAFETY: `buf` has `buf.len()` writable bytes; the call NUL-terminates.
        unsafe { ffi::rt_session_last_error(self.s, buf.as_mut_ptr(), buf.len()) };
        from_c_array(&buf)
    }

    fn check(&self, code: i32) -> Result<(), EngineError> {
        if code == ffi::RT_OK {
            return Ok(());
        }
        let msg = self.last_error();
        Err(match code {
            ffi::RT_E_ARG => EngineError::Arg(msg),
            ffi::RT_E_STATE => EngineError::State(msg),
            ffi::RT_E_DEVICE => EngineError::Device(msg),
            _ => EngineError::Internal(msg),
        })
    }

    fn read_device(&self) -> Result<Device, EngineError> {
        let mut d = RtDeviceDesc::zeroed();
        // SAFETY: `d` is a valid, writable rt_device_desc.
        self.check(unsafe { ffi::rt_session_device(self.s, &mut d) })?;
        Ok(Device {
            backend: from_c_array(&d.backend),
            sample_rate: d.sample_rate,
            block_frames: d.block_frames,
            channels: d.channels,
            claimed_rtt_ms: d.claimed_rtt_ms,
        })
    }

    fn read_strips(&self) -> Result<Vec<Strip>, EngineError> {
        // SAFETY: `self.s` is live.
        let count =
            unsafe { ffi::rt_session_strip_count(self.s) }.clamp(0, ffi::RT_MAX_STRIPS as i32);
        let mut strips = Vec::new();
        for s in 0..count {
            let mut d = RtStripDesc::zeroed();
            // SAFETY: `d` is a valid, writable rt_strip_desc.
            self.check(unsafe { ffi::rt_session_strip(self.s, s, &mut d) })?;
            let mut params = Vec::new();
            for p in 0..d.param_count.clamp(0, ffi::RT_MAX_PARAMS as i32) {
                let mut pd = RtParamDesc::zeroed();
                // SAFETY: `pd` is a valid, writable rt_param_desc.
                self.check(unsafe { ffi::rt_session_param(self.s, s, p, &mut pd) })?;
                params.push(Param {
                    id: from_c_ptr(pd.id),
                    name: from_c_ptr(pd.name),
                    unit: from_c_ptr(pd.unit),
                    min: pd.min,
                    max: pd.max,
                    default: pd.def,
                    taper: if pd.taper == ffi::RT_TAPER_LOG {
                        Taper::Log
                    } else {
                        Taper::Linear
                    },
                    read_only: pd.flags & ffi::RT_FLAG_READ_ONLY != 0,
                    toggle: pd.flags & ffi::RT_FLAG_TOGGLE != 0,
                });
            }
            strips.push(Strip {
                name: from_c_ptr(d.name),
                latency_frames: d.latency_frames,
                params,
            });
        }
        Ok(strips)
    }
}

impl Drop for FfiEngine {
    fn drop(&mut self) {
        // SAFETY: `self.s` came from rt_session_create and is destroyed exactly once.
        unsafe { ffi::rt_session_destroy(self.s) };
    }
}

impl Engine for FfiEngine {
    fn strips(&self) -> &[Strip] {
        &self.strips
    }

    fn device(&self) -> &Device {
        &self.device
    }

    fn set_param(&mut self, strip: usize, param: usize, v: f64) -> Result<(), EngineError> {
        let (s, p) = (index(strip)?, index(param)?);
        // SAFETY: `self.s` is live; indices are checked by rt_ffi.
        self.check(unsafe { ffi::rt_session_set_param(self.s, s, p, v) })
    }

    fn snapshot(&mut self) -> Result<Snapshot, EngineError> {
        let mut raw = Box::new(RtSnapshot::zeroed());
        // SAFETY: `raw` is a valid, writable rt_snapshot.
        self.check(unsafe { ffi::rt_session_snapshot(self.s, &mut *raw) })?;
        let ch = usize::try_from(raw.channels)
            .unwrap_or(0)
            .min(ffi::RT_MAX_CHANNELS);
        let params = self
            .strips
            .iter()
            .enumerate()
            .map(|(s, st)| raw.params[s][..st.params.len()].to_vec())
            .collect();
        Ok(Snapshot {
            callbacks: raw.callbacks,
            engine_xruns: raw.engine_xruns,
            device_xruns: raw.device_xruns,
            capture_overruns: raw.capture_overruns,
            in_clips: raw.in_clips,
            out_clips: raw.out_clips,
            deadline_ns: raw.deadline_ns,
            hist_window: Box::new(raw.hist_window),
            in_peak: raw.in_peak[..ch].to_vec(),
            out_peak: raw.out_peak[..ch].to_vec(),
            params,
            running: raw.running != 0,
            device_error: from_c_array(&raw.device_error),
        })
    }

    fn percentile_ns(&self, counts: &Histogram, p: f64) -> u64 {
        // SAFETY: `counts` holds exactly RT_HIST_BUCKETS values (asserted above).
        unsafe { ffi::rt_hist_percentile_ns(counts.as_ptr(), p) }
    }

    fn bucket_upper_ns(&self, bucket: usize) -> u64 {
        // SAFETY: pure function over an integer.
        unsafe { ffi::rt_hist_bucket_upper_ns(i32::try_from(bucket).unwrap_or(i32::MAX)) }
    }
}
