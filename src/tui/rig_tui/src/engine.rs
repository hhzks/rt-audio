use std::ffi::{CStr, CString, c_char};
use std::ptr;

use rig_ui::model::{
    Config, Device, DeviceEntry, Engine, EngineError, HIST_BUCKETS, Histogram, LatencyKind,
    LatencyPhase, LatencyRepeat, LatencySettings, LatencyState, LatencyStatus, Outcome, Param,
    Snapshot, Strip, Taper,
};

use crate::ffi::{
    self, RtConfigDesc, RtDeviceDesc, RtDeviceInfo, RtLatencySettings, RtLatencyStatus,
    RtOpenConfig, RtParamDesc, RtSession, RtSnapshot, RtStripDesc,
};

const _: () = assert!(HIST_BUCKETS == ffi::RT_HIST_BUCKETS);
const ENUMERATE_CAP: usize = 64;

pub struct OpenOptions {
    pub backend: Option<String>,
    pub input: Option<String>,
    pub output: Option<String>,
    pub rate: f64,
    pub block: i32,
    pub exclusive: bool,
    pub ring: f64,
}

pub struct FfiEngine {
    s: *mut RtSession,
    strips: Vec<Strip>,
    device: Device,
    config: Config,
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

fn c_string(s: Option<&str>) -> Result<Option<CString>, EngineError> {
    s.map(CString::new)
        .transpose()
        .map_err(|_| EngineError::Arg("argument contains a NUL byte".into()))
}

fn non_empty(s: &str) -> Option<&str> {
    (!s.is_empty()).then_some(s)
}

fn ptr_of(c: Option<&CString>) -> *const c_char {
    c.map_or(ptr::null(), |c| c.as_ptr())
}

fn index(i: usize) -> Result<i32, EngineError> {
    i32::try_from(i).map_err(|_| EngineError::Arg("index out of range".into()))
}

fn entry(d: &RtDeviceInfo) -> DeviceEntry {
    DeviceEntry {
        id: from_c_array(&d.id),
        name: from_c_array(&d.name),
        inputs: d.max_input_channels,
        outputs: d.max_output_channels,
        default_rate: d.default_sample_rate,
        default_in: d.is_default_input != 0,
        default_out: d.is_default_output != 0,
    }
}

fn latency_status_from(raw: &RtLatencyStatus) -> LatencyStatus {
    let state = match raw.state {
        ffi::RT_LAT_RUNNING => LatencyState::Running,
        ffi::RT_LAT_DONE => LatencyState::Done,
        ffi::RT_LAT_FAILED => LatencyState::Failed,
        ffi::RT_LAT_CANCELLED => LatencyState::Cancelled,
        _ => LatencyState::Idle,
    };
    let kept = usize::try_from(raw.kept)
        .unwrap_or(0)
        .min(ffi::RT_LAT_MAX_REPEATS);
    LatencyStatus {
        state,
        kind: if raw.kind == ffi::RT_LAT_MEASURE {
            LatencyKind::Measure
        } else {
            LatencyKind::Control
        },
        phase: if raw.phase == ffi::RT_LAT_PHASE_CHAIN {
            LatencyPhase::Chain
        } else {
            LatencyPhase::Direct
        },
        repeat: raw.repeat,
        repeats: raw.repeats,
        latency_mode: raw.latency_mode != 0,
        control_passed: raw.control_passed != 0,
        chain_valid: raw.chain_valid != 0,
        clipped: raw.clipped != 0,
        kept: raw.kept,
        discarded: raw.discarded,
        measured_ms: raw.measured_ms,
        spread_ms: raw.spread_ms,
        computed_ms: raw.computed_ms,
        chain_measured_ms: raw.chain_measured_ms,
        chain_reported_frames: raw.chain_reported_frames,
        direct: raw.direct[..kept]
            .iter()
            .map(|r| LatencyRepeat {
                lag_ms: r.lag_ms,
                correlation: r.correlation,
                psr: r.psr,
                valid: r.valid != 0,
            })
            .collect(),
        message: from_c_array(&raw.message),
    }
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
            config: Config::default(),
        };

        let backend = c_string(o.backend.as_deref())?;
        let input = c_string(o.input.as_deref())?;
        let output = c_string(o.output.as_deref())?;
        let cfg = RtOpenConfig {
            backend: ptr_of(backend.as_ref()),
            input_id: ptr_of(input.as_ref()),
            output_id: ptr_of(output.as_ref()),
            sample_rate: o.rate,
            block_frames: o.block,
            exclusive: u8::from(o.exclusive),
            ring_blocks: o.ring,
        };
        // SAFETY: `e.s` is live; `cfg` and the CStrings it points to outlive the call.
        e.check(unsafe { ffi::rt_session_open(e.s, &cfg) })?;
        e.device = e.read_device()?;
        e.config = e.read_config()?;
        e.strips = e.read_strips()?;
        Ok(e)
    }

    fn last_error(&self) -> String {
        let mut buf: [c_char; 512] = [0; 512];
        // SAFETY: `buf` has `buf.len()` writable bytes; the call NUL-terminates.
        unsafe { ffi::rt_session_last_error(self.s, buf.as_mut_ptr(), buf.len()) };
        from_c_array(&buf)
    }

    fn error(&self, code: i32) -> EngineError {
        let msg = self.last_error();
        match code {
            ffi::RT_E_ARG => EngineError::Arg(msg),
            ffi::RT_E_STATE => EngineError::State(msg),
            ffi::RT_E_DEVICE => EngineError::Device(msg),
            _ => EngineError::Internal(msg),
        }
    }

    fn check(&self, code: i32) -> Result<(), EngineError> {
        if code == ffi::RT_OK {
            Ok(())
        } else {
            Err(self.error(code))
        }
    }

    fn read_device(&self) -> Result<Device, EngineError> {
        let mut d = RtDeviceDesc::zeroed();
        // SAFETY: `d` is a valid, writable rt_device_desc.
        self.check(unsafe { ffi::rt_session_device(self.s, &mut d) })?;
        Ok(Device {
            backend: from_c_array(&d.backend),
            input: from_c_array(&d.input),
            output: from_c_array(&d.output),
            sample_rate: d.sample_rate,
            block_frames: d.block_frames,
            channels: d.channels,
            claimed_rtt_ms: d.claimed_rtt_ms,
        })
    }

    fn read_config(&self) -> Result<Config, EngineError> {
        let mut c = RtConfigDesc::zeroed();
        // SAFETY: `c` is a valid, writable rt_config_desc.
        self.check(unsafe { ffi::rt_session_config(self.s, &mut c) })?;
        Ok(Config {
            backend: from_c_array(&c.backend),
            input: from_c_array(&c.input_id),
            output: from_c_array(&c.output_id),
            rate: c.sample_rate,
            block: c.block_frames,
            exclusive: c.exclusive != 0,
            ring_blocks: c.ring_blocks,
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
            capture_underruns: raw.capture_underruns,
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

    fn devices(&mut self) -> Result<Vec<DeviceEntry>, EngineError> {
        let mut cap = ENUMERATE_CAP;
        loop {
            let mut buf: Vec<RtDeviceInfo> = (0..cap).map(|_| RtDeviceInfo::zeroed()).collect();
            let cap_c = i32::try_from(cap)
                .map_err(|_| EngineError::Internal("device list too long".into()))?;
            let mut total: i32 = 0;
            // SAFETY: `buf` has `cap` writable entries and `total` is writable.
            self.check(unsafe {
                ffi::rt_session_enumerate(self.s, buf.as_mut_ptr(), cap_c, &mut total)
            })?;
            let n = usize::try_from(total).unwrap_or(0);
            if n <= cap || cap > ENUMERATE_CAP {
                buf.truncate(n.min(cap));
                return Ok(buf.iter().map(entry).collect());
            }
            cap = n;
        }
    }

    fn config(&self) -> &Config {
        &self.config
    }

    fn reconfigure(&mut self, next: &Config) -> Result<Outcome, EngineError> {
        let input = c_string(non_empty(&next.input))?;
        let output = c_string(non_empty(&next.output))?;
        let cfg = RtOpenConfig {
            backend: ptr::null(),
            input_id: ptr_of(input.as_ref()),
            output_id: ptr_of(output.as_ref()),
            sample_rate: next.rate,
            block_frames: next.block,
            exclusive: u8::from(next.exclusive),
            ring_blocks: next.ring_blocks,
        };
        let mut outcome: i32 = -1;
        // SAFETY: `self.s` is live; `cfg`, the CStrings it points to and `outcome` outlive the call.
        let code = unsafe { ffi::rt_session_reconfigure(self.s, &cfg, &mut outcome) };
        let result = match (code, outcome) {
            (ffi::RT_OK, _) => Outcome::Applied,
            (ffi::RT_E_DEVICE, ffi::RT_RECONF_ROLLED_BACK) => {
                Outcome::RolledBack(self.last_error())
            }
            (ffi::RT_E_DEVICE, ffi::RT_RECONF_STOPPED) => Outcome::Stopped(self.last_error()),
            _ => return Err(self.error(code)),
        };
        self.config = self.read_config()?;
        if !matches!(result, Outcome::Stopped(_)) {
            self.device = self.read_device()?;
        }
        Ok(result)
    }

    fn control_panel(&mut self) -> Result<(), EngineError> {
        let mut result: i32 = -1;
        // SAFETY: `self.s` is live and `result` is writable.
        self.check(unsafe { ffi::rt_session_control_panel(self.s, &mut result) })
    }

    fn latency_enter(&mut self) -> Result<(), EngineError> {
        // SAFETY: `self.s` is live.
        self.check(unsafe { ffi::rt_session_latency_enter(self.s) })
    }

    fn latency_leave(&mut self) -> Result<(), EngineError> {
        // SAFETY: `self.s` is live.
        self.check(unsafe { ffi::rt_session_latency_leave(self.s) })
    }

    fn latency_start(&mut self, kind: LatencyKind, s: LatencySettings) -> Result<(), EngineError> {
        let settings = RtLatencySettings {
            repeats: s.repeats,
            amplitude: s.amplitude,
        };
        let k = match kind {
            LatencyKind::Control => ffi::RT_LAT_CONTROL,
            LatencyKind::Measure => ffi::RT_LAT_MEASURE,
        };
        // SAFETY: `self.s` is live; `settings` outlives the call.
        self.check(unsafe { ffi::rt_session_latency_start(self.s, k, &settings) })
    }

    fn latency_cancel(&mut self) -> Result<(), EngineError> {
        // SAFETY: `self.s` is live.
        self.check(unsafe { ffi::rt_session_latency_cancel(self.s) })
    }

    fn latency_status(&self) -> Result<LatencyStatus, EngineError> {
        let mut raw = Box::new(RtLatencyStatus::zeroed());
        // SAFETY: `raw` is a valid, writable rt_latency_status.
        self.check(unsafe { ffi::rt_session_latency_status(self.s, &mut *raw) })?;
        Ok(latency_status_from(&raw))
    }
}
