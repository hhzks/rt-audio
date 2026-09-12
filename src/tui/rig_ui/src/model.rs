use std::collections::HashSet;
use std::fmt;

pub const HIST_BUCKETS: usize = 322;
pub type Histogram = [u64; HIST_BUCKETS];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Taper {
    Linear,
    Log,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Param {
    pub id: String,
    pub name: String,
    pub unit: String,
    pub min: f64,
    pub max: f64,
    pub default: f64,
    pub taper: Taper,
    pub read_only: bool,
    pub toggle: bool,
}

impl Param {
    pub fn is_row(&self) -> bool {
        !self.read_only && !self.toggle
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct Strip {
    pub name: String,
    pub latency_frames: i32,
    pub params: Vec<Param>,
}

impl Strip {
    pub fn toggle_index(&self) -> Option<usize> {
        self.params.iter().position(|p| p.toggle)
    }

    pub fn param_index(&self, id: &str) -> Option<usize> {
        self.params.iter().position(|p| p.id == id)
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Device {
    pub backend: String,
    pub input: String,
    pub output: String,
    pub sample_rate: f64,
    pub block_frames: i32,
    pub channels: i32,
    pub claimed_rtt_ms: f64,
}

impl Device {
    pub fn block_ms(&self) -> f64 {
        if self.sample_rate > 0.0 {
            1000.0 * f64::from(self.block_frames) / self.sample_rate
        } else {
            0.0
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct DeviceEntry {
    pub id: String,
    pub name: String,
    pub inputs: i32,
    pub outputs: i32,
    pub default_rate: f64,
    pub default_in: bool,
    pub default_out: bool,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Config {
    pub backend: String,
    pub input: String,
    pub output: String,
    pub rate: f64,
    pub block: i32,
    pub exclusive: bool,
}

impl Config {
    pub fn command_line(&self) -> String {
        let mut s = format!("rt_rig --backend {}", self.backend);
        if !self.input.is_empty() {
            s += &format!(" --in '{}'", self.input);
        }
        if !self.output.is_empty() {
            s += &format!(" --out '{}'", self.output);
        }
        if self.rate != 48000.0 {
            s += &format!(" --rate {}", self.rate);
        }
        if self.block != 0 {
            s += &format!(" --block {}", self.block);
        }
        if self.exclusive {
            s += " --exclusive";
        }
        s
    }
}

#[derive(Clone, Debug, PartialEq)]
pub enum Outcome {
    Applied,
    RolledBack(String),
    Stopped(String),
}

#[derive(Clone, Debug, PartialEq)]
pub struct Snapshot {
    pub callbacks: u64,
    pub engine_xruns: u64,
    pub device_xruns: u64,
    pub capture_overruns: u64,
    pub capture_underruns: u64,
    pub in_clips: u64,
    pub out_clips: u64,
    pub deadline_ns: u64,
    pub hist_window: Box<Histogram>,
    pub in_peak: Vec<f32>,
    pub out_peak: Vec<f32>,
    pub params: Vec<Vec<f64>>,
    pub running: bool,
    pub device_error: String,
}

impl Snapshot {
    pub fn empty(strips: &[Strip], channels: usize) -> Self {
        Snapshot {
            callbacks: 0,
            engine_xruns: 0,
            device_xruns: 0,
            capture_overruns: 0,
            capture_underruns: 0,
            in_clips: 0,
            out_clips: 0,
            deadline_ns: 0,
            hist_window: Box::new([0; HIST_BUCKETS]),
            in_peak: vec![0.0; channels],
            out_peak: vec![0.0; channels],
            params: strips
                .iter()
                .map(|s| s.params.iter().map(|p| p.default).collect())
                .collect(),
            running: true,
            device_error: String::new(),
        }
    }

    pub fn dropouts(&self) -> u64 {
        self.engine_xruns + self.device_xruns + self.capture_overruns + self.capture_underruns
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum EngineError {
    Arg(String),
    State(String),
    Device(String),
    Internal(String),
}

impl fmt::Display for EngineError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            EngineError::Arg(m) => write!(f, "invalid argument: {m}"),
            EngineError::State(m) => write!(f, "invalid state: {m}"),
            EngineError::Device(m) => write!(f, "device error: {m}"),
            EngineError::Internal(m) => write!(f, "internal error: {m}"),
        }
    }
}

impl std::error::Error for EngineError {}

pub trait Engine {
    fn strips(&self) -> &[Strip];
    fn device(&self) -> &Device;
    fn set_param(&mut self, strip: usize, param: usize, v: f64) -> Result<(), EngineError>;
    fn snapshot(&mut self) -> Result<Snapshot, EngineError>;
    fn percentile_ns(&self, counts: &Histogram, p: f64) -> u64;
    fn bucket_upper_ns(&self, bucket: usize) -> u64;
    fn devices(&mut self) -> Result<Vec<DeviceEntry>, EngineError>;
    fn config(&self) -> &Config;
    fn reconfigure(&mut self, next: &Config) -> Result<Outcome, EngineError>;
}

fn param(
    id: &str,
    name: &str,
    unit: &str,
    min: f64,
    max: f64,
    default: f64,
    taper: Taper,
) -> Param {
    Param {
        id: id.into(),
        name: name.into(),
        unit: unit.into(),
        min,
        max,
        default,
        taper,
        read_only: false,
        toggle: false,
    }
}

fn toggle(id: &str, default: f64) -> Param {
    Param {
        toggle: true,
        ..param(id, id, "", 0.0, 1.0, default, Taper::Linear)
    }
}

fn read_only(id: &str, name: &str, unit: &str, min: f64, max: f64) -> Param {
    Param {
        read_only: true,
        ..param(id, name, unit, min, max, max, Taper::Linear)
    }
}

pub struct FakeEngine {
    pub strips: Vec<Strip>,
    pub device: Device,
    pub values: Vec<Vec<f64>>,
    pub next: Snapshot,
    pub sets: Vec<(usize, usize, f64)>,
    pub fail_next_set: Option<EngineError>,
    pub devices: Vec<DeviceEntry>,
    pub config: Config,
    pub fail_ids: HashSet<String>,
    pub reconfigures: Vec<Config>,
    pub fail_devices: Option<EngineError>,
}

fn entry(id: &str, name: &str, inputs: i32, outputs: i32) -> DeviceEntry {
    DeviceEntry {
        id: id.into(),
        name: name.into(),
        inputs,
        outputs,
        default_rate: 48000.0,
        default_in: id == "mic",
        default_out: id == "phones",
    }
}

impl FakeEngine {
    pub fn rig() -> Self {
        use Taper::{Linear, Log};
        let strips = vec![
            Strip {
                name: "Master".into(),
                latency_frames: 0,
                params: vec![
                    param("in_gain", "in", "dB", -24.0, 24.0, 0.0, Linear),
                    param("out_gain", "out", "dB", -60.0, 12.0, 0.0, Linear),
                    toggle("bypass", 0.0),
                ],
            },
            Strip {
                name: "High-pass".into(),
                latency_frames: 0,
                params: vec![
                    param("freq", "freq", "Hz", 20.0, 20000.0, 80.0, Log),
                    param("q", "Q", "", 0.1, 10.0, 0.707, Log),
                ],
            },
            Strip {
                name: "Gate".into(),
                latency_frames: 0,
                params: vec![
                    toggle("on", 1.0),
                    param("threshold", "thr", "dB", -90.0, 0.0, -45.0, Linear),
                    read_only("gain_reduction", "gr", "dB", -26.0, 0.0),
                ],
            },
            Strip {
                name: "Drive".into(),
                latency_frames: 16,
                params: vec![
                    toggle("on", 1.0),
                    param("drive", "drive", "x", 1.0, 20.0, 1.0, Log),
                    param("mix", "mix", "%", 0.0, 1.0, 0.0, Linear),
                ],
            },
            Strip {
                name: "Low shelf".into(),
                latency_frames: 0,
                params: vec![
                    param("freq", "freq", "Hz", 20.0, 20000.0, 200.0, Log),
                    param("q", "Q", "", 0.1, 10.0, 0.707, Log),
                    param("gain", "gain", "dB", -24.0, 24.0, 2.0, Linear),
                ],
            },
        ];
        let device = Device {
            backend: "Null".into(),
            input: "synthetic tone".into(),
            output: "discard".into(),
            sample_rate: 48000.0,
            block_frames: 128,
            channels: 2,
            claimed_rtt_ms: 5.3,
        };
        let next = Snapshot {
            deadline_ns: 2_666_666,
            ..Snapshot::empty(&strips, 2)
        };
        let values = next.params.clone();
        FakeEngine {
            strips,
            device,
            values,
            next,
            sets: Vec::new(),
            fail_next_set: None,
            devices: vec![
                entry("mic", "Mic", 2, 0),
                entry("phones", "Phones", 0, 2),
                entry("usb", "USB Interface", 2, 2),
            ],
            config: Config {
                backend: "null".into(),
                input: String::new(),
                output: String::new(),
                rate: 48000.0,
                block: 128,
                exclusive: false,
            },
            fail_ids: HashSet::new(),
            reconfigures: Vec::new(),
            fail_devices: None,
        }
    }

    fn fails(&self, c: &Config) -> bool {
        self.fail_ids.contains(&c.input) || self.fail_ids.contains(&c.output)
    }

    fn stop(&mut self, why: &str) -> Outcome {
        self.next.running = false;
        self.next.device_error = why.to_owned();
        Outcome::Stopped(why.to_owned())
    }
}

impl Engine for FakeEngine {
    fn strips(&self) -> &[Strip] {
        &self.strips
    }

    fn device(&self) -> &Device {
        &self.device
    }

    fn set_param(&mut self, strip: usize, param: usize, v: f64) -> Result<(), EngineError> {
        self.sets.push((strip, param, v));
        if let Some(e) = self.fail_next_set.take() {
            return Err(e);
        }
        let p = self
            .strips
            .get(strip)
            .and_then(|s| s.params.get(param))
            .ok_or_else(|| EngineError::Arg("index out of range".into()))?;
        if p.read_only || !v.is_finite() {
            return Err(EngineError::Arg("value rejected".into()));
        }
        let v = if p.toggle {
            if v >= 0.5 { 1.0 } else { 0.0 }
        } else {
            v.clamp(p.min, p.max)
        };
        self.values[strip][param] = v;
        Ok(())
    }

    fn snapshot(&mut self) -> Result<Snapshot, EngineError> {
        Ok(Snapshot {
            params: self.values.clone(),
            ..self.next.clone()
        })
    }

    fn percentile_ns(&self, counts: &Histogram, p: f64) -> u64 {
        let total: u64 = counts.iter().sum();
        if total == 0 {
            return 0;
        }
        let threshold = p.clamp(0.0, 1.0) * total as f64;
        let mut cum = 0u64;
        for (b, &c) in counts.iter().enumerate() {
            cum += c;
            if c != 0 && cum as f64 >= threshold {
                return self.bucket_upper_ns(b);
            }
        }
        0
    }

    fn bucket_upper_ns(&self, bucket: usize) -> u64 {
        (bucket as u64 + 1) * 1000
    }

    fn devices(&mut self) -> Result<Vec<DeviceEntry>, EngineError> {
        match &self.fail_devices {
            Some(e) => Err(e.clone()),
            None => Ok(self.devices.clone()),
        }
    }

    fn config(&self) -> &Config {
        &self.config
    }

    fn reconfigure(&mut self, next: &Config) -> Result<Outcome, EngineError> {
        self.reconfigures.push(next.clone());
        if !self.fails(next) {
            self.config = next.clone();
            self.next.running = true;
            self.next.device_error.clear();
            return Ok(Outcome::Applied);
        }
        if *next == self.config {
            return Ok(self.stop("could not reopen the device"));
        }
        if !self.fails(&self.config) {
            return Ok(Outcome::RolledBack(
                "could not open the new config; restored the previous config".into(),
            ));
        }
        Ok(self.stop("could not open the new config; could not restore the previous config"))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fake_rig_mirrors_the_cpp_rig() {
        let e = FakeEngine::rig();
        let names: Vec<&str> = e.strips.iter().map(|s| s.name.as_str()).collect();
        assert_eq!(names, ["Master", "High-pass", "Gate", "Drive", "Low shelf"]);
        let counts: Vec<usize> = e.strips.iter().map(|s| s.params.len()).collect();
        assert_eq!(counts, [3, 2, 3, 3, 3]);
        assert_eq!(e.strips[3].latency_frames, 16);
        assert_eq!(e.strips[2].toggle_index(), Some(0));
        assert_eq!(e.strips[0].toggle_index(), Some(2));
        assert_eq!(e.strips[1].toggle_index(), None);
        assert!(e.strips[2].params[2].read_only);
        assert!(!e.strips[2].params[2].is_row());
        for s in &e.strips {
            for p in s.params.iter().filter(|p| p.is_row()) {
                assert!(p.name.chars().count() <= 5, "{}", p.name);
            }
        }
    }

    #[test]
    fn fake_set_param_clamps_like_the_engine() {
        let mut e = FakeEngine::rig();
        e.set_param(3, 1, 100.0).unwrap();
        assert_eq!(e.values[3][1], 20.0);
        e.set_param(2, 0, 0.2).unwrap();
        assert_eq!(e.values[2][0], 0.0);
        assert!(matches!(e.set_param(2, 2, -10.0), Err(EngineError::Arg(_))));
        assert!(matches!(
            e.set_param(3, 1, f64::NAN),
            Err(EngineError::Arg(_))
        ));
        assert!(matches!(e.set_param(9, 0, 0.0), Err(EngineError::Arg(_))));
        assert_eq!(e.sets.len(), 5);
    }

    #[test]
    fn fake_snapshot_reports_current_values() {
        let mut e = FakeEngine::rig();
        e.set_param(0, 1, -3.0).unwrap();
        let s = e.snapshot().unwrap();
        assert_eq!(s.params[0][1], -3.0);
        assert_eq!(s.in_peak.len(), 2);
        assert!(s.running);
    }

    #[test]
    fn dropouts_add_every_counter() {
        let mut s = Snapshot::empty(&FakeEngine::rig().strips, 2);
        s.engine_xruns = 1;
        s.device_xruns = 2;
        s.capture_overruns = 4;
        s.capture_underruns = 8;
        assert_eq!(s.dropouts(), 15);
    }

    #[test]
    fn block_ms() {
        let d = Device {
            sample_rate: 48000.0,
            block_frames: 128,
            ..Device::default()
        };
        assert!((d.block_ms() - 2.6666666).abs() < 1e-6);
        assert_eq!(Device::default().block_ms(), 0.0);
    }

    fn cfg(backend: &str, input: &str) -> Config {
        Config {
            backend: backend.into(),
            input: input.into(),
            output: String::new(),
            rate: 48000.0,
            block: 0,
            exclusive: false,
        }
    }

    #[test]
    fn command_line_lists_only_non_defaults() {
        let c = Config {
            block: 128,
            exclusive: true,
            ..cfg("wasapi", "{0.0.1}.{a}")
        };
        assert_eq!(
            c.command_line(),
            "rt_rig --backend wasapi --in '{0.0.1}.{a}' --block 128 --exclusive"
        );
        let d = Config {
            rate: 44100.0,
            output: "hw:1".into(),
            ..cfg("alsa", "")
        };
        assert_eq!(
            d.command_line(),
            "rt_rig --backend alsa --out 'hw:1' --rate 44100"
        );
    }

    #[test]
    fn fake_reconfigure_follows_the_session_rules() {
        let mut e = FakeEngine::rig();
        let b = Config {
            input: "mic".into(),
            ..e.config.clone()
        };
        assert_eq!(e.reconfigure(&b).unwrap(), Outcome::Applied);
        assert_eq!(e.config, b);

        e.fail_ids.insert("usb".into());
        let c = Config {
            input: "usb".into(),
            ..b.clone()
        };
        assert!(matches!(e.reconfigure(&c).unwrap(), Outcome::RolledBack(_)));
        assert_eq!(e.config, b);

        e.fail_ids.insert("mic".into());
        assert!(matches!(e.reconfigure(&c).unwrap(), Outcome::Stopped(_)));
        assert!(!e.snapshot().unwrap().running);
        assert!(matches!(e.reconfigure(&b).unwrap(), Outcome::Stopped(m) if m.contains("reopen")));

        e.fail_ids.clear();
        assert_eq!(e.reconfigure(&b).unwrap(), Outcome::Applied);
        assert!(e.snapshot().unwrap().running);
        assert_eq!(e.reconfigures.len(), 5);
    }

    #[test]
    fn fake_devices_can_fail() {
        let mut e = FakeEngine::rig();
        assert_eq!(e.devices().unwrap().len(), 3);
        e.fail_devices = Some(EngineError::Device("boom".into()));
        assert!(e.devices().is_err());
    }
}
