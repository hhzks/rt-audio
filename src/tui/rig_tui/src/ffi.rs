use std::ffi::c_char;
use std::mem::offset_of;

pub const RT_OK: i32 = 0;
pub const RT_E_ARG: i32 = 1;
pub const RT_E_STATE: i32 = 2;
pub const RT_E_DEVICE: i32 = 3;
pub const RT_E_INTERNAL: i32 = 4;

pub const RT_MAX_CHANNELS: usize = 8;
pub const RT_MAX_STRIPS: usize = 16;
pub const RT_MAX_PARAMS: usize = 8;
pub const RT_HIST_BUCKETS: usize = 322;

pub const RT_TAPER_LOG: u8 = 1;
pub const RT_FLAG_READ_ONLY: u8 = 1;
pub const RT_FLAG_TOGGLE: u8 = 2;

pub const RT_RECONF_APPLIED: i32 = 0;
pub const RT_RECONF_ROLLED_BACK: i32 = 1;
pub const RT_RECONF_STOPPED: i32 = 2;

pub const RT_PANEL_MODAL: i32 = 1;
pub const RT_PANEL_ALREADY_OPEN: i32 = 2;
pub const RT_PANEL_NONE: i32 = 3;
pub const RT_ID_BYTES: usize = 256;
pub const RT_NAME_BYTES: usize = 128;

pub const RT_LAT_CONTROL: i32 = 0;
pub const RT_LAT_MEASURE: i32 = 1;
pub const RT_LAT_RUNNING: i32 = 1;
pub const RT_LAT_DONE: i32 = 2;
pub const RT_LAT_FAILED: i32 = 3;
pub const RT_LAT_CANCELLED: i32 = 4;
pub const RT_LAT_PHASE_CHAIN: i32 = 1;
pub const RT_LAT_MAX_REPEATS: usize = 16;

#[repr(C)]
pub struct RtSession {
    _private: [u8; 0],
}

#[repr(C)]
pub struct RtOpenConfig {
    pub backend: *const c_char,
    pub input_id: *const c_char,
    pub output_id: *const c_char,
    pub sample_rate: f64,
    pub block_frames: i32,
    pub exclusive: u8,
    pub ring_blocks: f64,
}

#[repr(C)]
pub struct RtParamDesc {
    pub id: *const c_char,
    pub name: *const c_char,
    pub unit: *const c_char,
    pub min: f64,
    pub max: f64,
    pub def: f64,
    pub taper: u8,
    pub flags: u8,
}

#[repr(C)]
pub struct RtStripDesc {
    pub name: *const c_char,
    pub param_count: i32,
    pub latency_frames: i32,
}

#[repr(C)]
pub struct RtDeviceDesc {
    pub backend: [c_char; 64],
    pub input: [c_char; 128],
    pub output: [c_char; 128],
    pub sample_rate: f64,
    pub claimed_rtt_ms: f64,
    pub block_frames: i32,
    pub channels: i32,
}

#[repr(C)]
pub struct RtSnapshot {
    pub callbacks: u64,
    pub engine_xruns: u64,
    pub device_xruns: u64,
    pub capture_overruns: u64,
    pub capture_underruns: u64,
    pub in_clips: u64,
    pub out_clips: u64,
    pub deadline_ns: u64,
    pub hist_window: [u64; RT_HIST_BUCKETS],
    pub in_peak: [f32; RT_MAX_CHANNELS],
    pub out_peak: [f32; RT_MAX_CHANNELS],
    pub params: [[f64; RT_MAX_PARAMS]; RT_MAX_STRIPS],
    pub channels: i32,
    pub running: u8,
    pub panel_open: u8,
    pub device_error: [c_char; 256],
}

#[repr(C)]
pub struct RtDeviceInfo {
    pub id: [c_char; RT_ID_BYTES],
    pub name: [c_char; RT_NAME_BYTES],
    pub max_input_channels: i32,
    pub max_output_channels: i32,
    pub default_sample_rate: f64,
    pub is_default_input: u8,
    pub is_default_output: u8,
}

#[repr(C)]
pub struct RtConfigDesc {
    pub backend: [c_char; 16],
    pub input_id: [c_char; RT_ID_BYTES],
    pub output_id: [c_char; RT_ID_BYTES],
    pub sample_rate: f64,
    pub block_frames: i32,
    pub exclusive: u8,
    pub ring_blocks: f64,
}

#[repr(C)]
pub struct RtLatencySettings {
    pub repeats: i32,
    pub amplitude: f32,
}

#[repr(C)]
pub struct RtLatencyRepeat {
    pub lag_ms: f64,
    pub correlation: f64,
    pub psr: f64,
    pub valid: u8,
    pub polarity_inverted: u8,
}

#[repr(C)]
pub struct RtLatencyStatus {
    pub state: i32,
    pub kind: i32,
    pub phase: i32,
    pub repeat: i32,
    pub repeats: i32,
    pub latency_mode: u8,
    pub control_passed: u8,
    pub chain_valid: u8,
    pub clipped: u8,
    pub kept: i32,
    pub discarded: i32,
    pub measured_ms: f64,
    pub spread_ms: f64,
    pub computed_ms: f64,
    pub chain_measured_ms: f64,
    pub chain_reported_frames: i32,
    pub direct: [RtLatencyRepeat; RT_LAT_MAX_REPEATS],
    pub message: [c_char; 256],
}

macro_rules! zeroed_ctor {
    ($($t:ty),*) => {$(
        impl $t {
            pub fn zeroed() -> Self {
                // SAFETY: every field is an integer, a float, an array of those,
                // or a raw pointer; all-zero bytes are a valid value for each.
                unsafe { std::mem::zeroed() }
            }
        }
    )*};
}
zeroed_ctor!(
    RtParamDesc,
    RtStripDesc,
    RtDeviceDesc,
    RtSnapshot,
    RtDeviceInfo,
    RtConfigDesc,
    RtLatencyStatus
);

unsafe extern "C" {
    pub fn rt_session_create() -> *mut RtSession;
    pub fn rt_session_destroy(s: *mut RtSession);
    pub fn rt_session_open(s: *mut RtSession, cfg: *const RtOpenConfig) -> i32;
    pub fn rt_session_stop(s: *mut RtSession) -> i32;
    pub fn rt_session_last_error(s: *const RtSession, buf: *mut c_char, cap: usize) -> usize;
    pub fn rt_session_device(s: *const RtSession, out: *mut RtDeviceDesc) -> i32;
    pub fn rt_session_strip_count(s: *const RtSession) -> i32;
    pub fn rt_session_strip(s: *const RtSession, strip: i32, out: *mut RtStripDesc) -> i32;
    pub fn rt_session_param(
        s: *const RtSession,
        strip: i32,
        param: i32,
        out: *mut RtParamDesc,
    ) -> i32;
    pub fn rt_session_set_param(s: *mut RtSession, strip: i32, param: i32, value: f64) -> i32;
    pub fn rt_session_snapshot(s: *mut RtSession, out: *mut RtSnapshot) -> i32;
    pub fn rt_session_enumerate(
        s: *mut RtSession,
        out: *mut RtDeviceInfo,
        cap: i32,
        total: *mut i32,
    ) -> i32;
    pub fn rt_session_config(s: *const RtSession, out: *mut RtConfigDesc) -> i32;
    pub fn rt_session_reconfigure(
        s: *mut RtSession,
        cfg: *const RtOpenConfig,
        outcome: *mut i32,
    ) -> i32;
    pub fn rt_session_control_panel(s: *mut RtSession, result: *mut i32) -> i32;
    pub fn rt_session_latency_enter(s: *mut RtSession) -> i32;
    pub fn rt_session_latency_leave(s: *mut RtSession) -> i32;
    pub fn rt_session_latency_start(
        s: *mut RtSession,
        kind: i32,
        settings: *const RtLatencySettings,
    ) -> i32;
    pub fn rt_session_latency_cancel(s: *mut RtSession) -> i32;
    pub fn rt_session_latency_status(s: *const RtSession, out: *mut RtLatencyStatus) -> i32;
    pub fn rt_hist_percentile_ns(counts: *const u64, p: f64) -> u64;
    pub fn rt_hist_bucket_upper_ns(bucket: i32) -> u64;
}

macro_rules! layout {
    ($v:ident, $t:ty; $($f:ident),*) => {
        $v.push(size_of::<$t>() as u64);
        $v.push(align_of::<$t>() as u64);
        $( $v.push(offset_of!($t, $f) as u64); )*
    };
}

// Same order as cLayout() in tests/test_ffi_layout.cpp.
pub fn layout_values() -> Vec<u64> {
    let mut v = Vec::new();
    layout!(v, RtOpenConfig; backend, input_id, output_id, sample_rate, block_frames, exclusive, ring_blocks);
    layout!(v, RtParamDesc; id, name, unit, min, max, def, taper, flags);
    layout!(v, RtStripDesc; name, param_count, latency_frames);
    layout!(v, RtDeviceDesc; backend, input, output, sample_rate, claimed_rtt_ms, block_frames, channels);
    layout!(v, RtSnapshot; callbacks, engine_xruns, device_xruns, capture_overruns,
        capture_underruns, in_clips, out_clips, deadline_ns, hist_window, in_peak, out_peak,
        params, channels, running, panel_open, device_error);
    layout!(v, RtDeviceInfo; id, name, max_input_channels, max_output_channels,
        default_sample_rate, is_default_input, is_default_output);
    layout!(v, RtConfigDesc; backend, input_id, output_id, sample_rate, block_frames, exclusive, ring_blocks);
    layout!(v, RtLatencySettings; repeats, amplitude);
    layout!(v, RtLatencyRepeat; lag_ms, correlation, psr, valid, polarity_inverted);
    layout!(v, RtLatencyStatus; state, kind, phase, repeat, repeats, latency_mode,
        control_passed, chain_valid, clipped, kept, discarded, measured_ms, spread_ms,
        computed_ms, chain_measured_ms, chain_reported_frames, direct, message);
    v
}
