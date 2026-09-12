use crate::model::{Config, Device, LatencyKind, LatencyState, LatencyStatus};
use crate::picker::khz;

pub const LEVELS_DB: [f64; 9] = [-24.0, -21.0, -18.0, -15.0, -12.0, -9.0, -6.0, -3.0, 0.0];
const DEFAULT_LEVEL: usize = 6;
pub const UNSTABLE_MS: f64 = 1.0;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Step {
    Home,
    ConfirmControl,
    ConfirmMeasure,
    Running,
    ConfirmLeave,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Key {
    Control,
    Enter,
    Esc,
    LevelDown,
    LevelUp,
    Devices,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Action {
    None,
    Start(LatencyKind),
    Cancel,
    Leave,
    OpenPicker,
}

#[derive(Clone, Debug, PartialEq)]
pub struct LatencyRow {
    pub label: String,
    pub measured_ms: f64,
    pub spread_ms: f64,
    pub computed_ms: f64,
    pub chain_ms: Option<f64>,
    pub unstable: bool,
    pub clipped: bool,
}

impl LatencyRow {
    pub fn new(config: &Config, device: &Device, status: &LatencyStatus) -> Self {
        let mode = match config.backend.as_str() {
            "wasapi" if config.exclusive => "excl",
            "wasapi" => "shared",
            other => other,
        };
        LatencyRow {
            label: format!(
                "{mode} {} fr {}",
                device.block_frames,
                khz(device.sample_rate)
            ),
            measured_ms: status.measured_ms,
            spread_ms: status.spread_ms,
            computed_ms: status.computed_ms,
            chain_ms: status.chain_valid.then_some(status.chain_measured_ms),
            unstable: status.spread_ms > UNSTABLE_MS,
            clipped: status.clipped,
        }
    }

    pub fn unaccounted_ms(&self) -> f64 {
        self.measured_ms - self.computed_ms
    }
}

pub struct LatencyScreen {
    pub step: Step,
    pub level: usize,
    pub status: LatencyStatus,
    pub message: Option<String>,
    started_with: Option<(Config, Device)>,
}

impl Default for LatencyScreen {
    fn default() -> Self {
        Self::new()
    }
}

impl LatencyScreen {
    pub fn new() -> Self {
        LatencyScreen {
            step: Step::Home,
            level: DEFAULT_LEVEL,
            status: LatencyStatus {
                latency_mode: true,
                ..LatencyStatus::default()
            },
            message: None,
            started_with: None,
        }
    }

    pub fn level_db(&self) -> f64 {
        LEVELS_DB[self.level]
    }

    pub fn amplitude(&self) -> f32 {
        10f64.powf(self.level_db() / 20.0) as f32
    }

    pub fn key(&mut self, key: Key, device_running: bool) -> Action {
        match (self.step, key) {
            (Step::Home, Key::Control) => self.confirm(Step::ConfirmControl, device_running),
            (Step::Home, Key::Enter) => {
                if device_running && !self.status.control_passed {
                    self.message = Some("run the negative control first (c)".into());
                    Action::None
                } else {
                    self.confirm(Step::ConfirmMeasure, device_running)
                }
            }
            (Step::Home, Key::Esc) => {
                self.step = Step::ConfirmLeave;
                Action::None
            }
            (Step::Home, Key::LevelDown) => {
                self.level = self.level.saturating_sub(1);
                Action::None
            }
            (Step::Home, Key::LevelUp) => {
                self.level = (self.level + 1).min(LEVELS_DB.len() - 1);
                Action::None
            }
            (Step::Home, Key::Devices) => Action::OpenPicker,
            (Step::ConfirmControl, Key::Enter) => Action::Start(LatencyKind::Control),
            (Step::ConfirmMeasure, Key::Enter) => Action::Start(LatencyKind::Measure),
            (Step::ConfirmLeave, Key::Enter) => Action::Leave,
            (Step::ConfirmControl | Step::ConfirmMeasure | Step::ConfirmLeave, Key::Esc) => {
                self.step = Step::Home;
                Action::None
            }
            (Step::Running, Key::Esc) => Action::Cancel,
            _ => Action::None,
        }
    }

    fn confirm(&mut self, step: Step, device_running: bool) -> Action {
        if device_running {
            self.step = step;
            self.message = None;
        } else {
            self.message = Some("the device is stopped".into());
        }
        Action::None
    }

    pub fn started(&mut self, config: Config, device: Device) {
        self.started_with = Some((config, device));
        self.step = Step::Running;
        self.message = None;
    }

    pub fn ingest(&mut self, status: LatencyStatus) -> Option<LatencyRow> {
        let mut row = None;
        if self.step == Step::Running && status.state != LatencyState::Running {
            self.step = Step::Home;
            let started = self.started_with.take();
            if status.state == LatencyState::Done && status.kind == LatencyKind::Measure {
                row = started.map(|(c, d)| LatencyRow::new(&c, &d, &status));
            }
            self.message = match status.state {
                LatencyState::Cancelled => Some("cancelled".into()),
                _ if status.message.is_empty() => None,
                _ => Some(status.message.clone()),
            };
        }
        self.status = status;
        row
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn done_measure() -> LatencyStatus {
        LatencyStatus {
            state: LatencyState::Done,
            kind: LatencyKind::Measure,
            latency_mode: true,
            control_passed: true,
            measured_ms: 21.89,
            spread_ms: 0.06,
            computed_ms: 6.33,
            chain_valid: true,
            chain_measured_ms: 0.41,
            ..LatencyStatus::default()
        }
    }

    fn wasapi(exclusive: bool) -> Config {
        Config {
            backend: "wasapi".into(),
            input: String::new(),
            output: String::new(),
            rate: 48000.0,
            block: 0,
            exclusive,
        }
    }

    fn device(block: i32, rate: f64) -> Device {
        Device {
            block_frames: block,
            sample_rate: rate,
            channels: 2,
            ..Device::default()
        }
    }

    #[test]
    fn every_run_needs_a_confirm_step() {
        let mut s = LatencyScreen::new();
        assert_eq!(s.key(Key::Control, true), Action::None);
        assert_eq!(s.step, Step::ConfirmControl);
        assert_eq!(s.key(Key::Esc, true), Action::None);
        assert_eq!(s.step, Step::Home);
        s.key(Key::Control, true);
        assert_eq!(s.key(Key::Enter, true), Action::Start(LatencyKind::Control));

        let mut s = LatencyScreen::new();
        s.status.control_passed = true;
        assert_eq!(s.key(Key::Enter, true), Action::None);
        assert_eq!(s.step, Step::ConfirmMeasure);
        assert_eq!(s.key(Key::Enter, true), Action::Start(LatencyKind::Measure));
    }

    #[test]
    fn measure_needs_a_control_and_a_running_device() {
        let mut s = LatencyScreen::new();
        assert_eq!(s.key(Key::Enter, true), Action::None);
        assert_eq!(s.step, Step::Home);
        assert_eq!(
            s.message.as_deref(),
            Some("run the negative control first (c)")
        );
        s.key(Key::Control, false);
        assert_eq!(s.step, Step::Home);
        assert_eq!(s.message.as_deref(), Some("the device is stopped"));
    }

    #[test]
    fn escape_by_step() {
        let mut s = LatencyScreen::new();
        s.key(Key::Esc, true);
        assert_eq!(s.step, Step::ConfirmLeave);
        assert_eq!(s.key(Key::Enter, true), Action::Leave);

        let mut s = LatencyScreen::new();
        s.started(wasapi(false), device(128, 48000.0));
        assert_eq!(s.key(Key::Esc, true), Action::Cancel);
        assert_eq!(s.key(Key::Control, true), Action::None);
        assert_eq!(s.key(Key::Devices, true), Action::None);
        assert_eq!(s.step, Step::Running);
    }

    #[test]
    fn level_steps_stop_at_the_ends() {
        let mut s = LatencyScreen::new();
        assert_eq!(s.level_db(), -6.0);
        assert!((s.amplitude() - 0.501).abs() < 0.001);
        for _ in 0..20 {
            s.key(Key::LevelUp, true);
        }
        assert_eq!(s.level_db(), 0.0);
        assert_eq!(s.amplitude(), 1.0);
        for _ in 0..20 {
            s.key(Key::LevelDown, true);
        }
        assert_eq!(s.level_db(), -24.0);
    }

    #[test]
    fn a_done_measure_gives_exactly_one_row() {
        let mut s = LatencyScreen::new();
        s.started(wasapi(true), device(144, 48000.0));
        let running = LatencyStatus {
            state: LatencyState::Running,
            kind: LatencyKind::Measure,
            ..LatencyStatus::default()
        };
        assert!(s.ingest(running).is_none());
        let row = s.ingest(done_measure()).expect("one row");
        assert_eq!(row.label, "excl 144 fr 48 kHz");
        assert_eq!(row.chain_ms, Some(0.41));
        assert!(!row.unstable);
        assert!((row.unaccounted_ms() - 15.56).abs() < 1e-9);
        assert_eq!(s.step, Step::Home);
        assert!(s.ingest(done_measure()).is_none());
    }

    #[test]
    fn a_control_gives_no_row_and_failures_give_the_message() {
        let mut s = LatencyScreen::new();
        s.started(wasapi(false), device(1056, 48000.0));
        let failed = LatencyStatus {
            state: LatencyState::Failed,
            kind: LatencyKind::Control,
            message: "detected a peak at 4.02 ms".into(),
            ..LatencyStatus::default()
        };
        assert!(s.ingest(failed).is_none());
        assert_eq!(s.message.as_deref(), Some("detected a peak at 4.02 ms"));

        s.started(wasapi(false), device(1056, 48000.0));
        let passed = LatencyStatus {
            state: LatencyState::Done,
            kind: LatencyKind::Control,
            control_passed: true,
            ..LatencyStatus::default()
        };
        assert!(s.ingest(passed).is_none());

        s.started(wasapi(false), device(1056, 48000.0));
        s.ingest(LatencyStatus {
            state: LatencyState::Cancelled,
            ..LatencyStatus::default()
        });
        assert_eq!(s.message.as_deref(), Some("cancelled"));
        assert_eq!(s.step, Step::Home);
    }

    #[test]
    fn row_labels_and_flags() {
        let shared = LatencyRow::new(&wasapi(false), &device(1056, 48000.0), &done_measure());
        assert_eq!(shared.label, "shared 1056 fr 48 kHz");

        let alsa = Config {
            backend: "alsa".into(),
            ..wasapi(false)
        };
        let noisy = LatencyStatus {
            spread_ms: 1.5,
            clipped: true,
            ..done_measure()
        };
        let row = LatencyRow::new(&alsa, &device(256, 44100.0), &noisy);
        assert_eq!(row.label, "alsa 256 fr 44.1 kHz");
        assert!(row.unstable);
        assert!(row.clipped);

        let no_chain = LatencyStatus {
            chain_valid: false,
            ..done_measure()
        };
        assert_eq!(
            LatencyRow::new(&alsa, &device(256, 48000.0), &no_chain).chain_ms,
            None
        );
    }
}
