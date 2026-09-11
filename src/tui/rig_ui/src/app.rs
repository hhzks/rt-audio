use std::time::{Duration, Instant};

use ratatui::crossterm::event::{Event, KeyCode, KeyEventKind, KeyModifiers};

use crate::meters::{ClipLatch, Meter};
use crate::model::{Device, Engine, EngineError, HIST_BUCKETS, Snapshot, Strip};
use crate::stats::{Stats, Status};
use crate::taper;

pub const QUIT_WINDOW: Duration = Duration::from_millis(1500);
pub const MESSAGE_TIME: Duration = Duration::from_secs(2);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Msg {
    Up,
    Down,
    Coarse(i8),
    Fine(i8),
    JumpStrip(usize),
    ToggleSelected,
    ToggleBypass,
    ToggleDrive,
    ToggleGate,
    ResetParam,
    ResetStats,
    SwapPanel,
    Help,
    CloseOverlay,
    Quit,
    QuitNow,
}

pub fn map_key(ev: &Event) -> Option<Msg> {
    let Event::Key(k) = ev else { return None };
    if !matches!(k.kind, KeyEventKind::Press | KeyEventKind::Repeat) {
        return None;
    }
    if k.modifiers.contains(KeyModifiers::CONTROL) {
        return matches!(k.code, KeyCode::Char('c')).then_some(Msg::QuitNow);
    }
    Some(match k.code {
        KeyCode::Up | KeyCode::Char('k') => Msg::Up,
        KeyCode::Down | KeyCode::Char('j') => Msg::Down,
        KeyCode::Left | KeyCode::Char('h') => Msg::Coarse(-1),
        KeyCode::Right | KeyCode::Char('l') => Msg::Coarse(1),
        KeyCode::Char('[') => Msg::Fine(-1),
        KeyCode::Char(']') => Msg::Fine(1),
        KeyCode::Char(' ') => Msg::ToggleBypass,
        KeyCode::Char('d') => Msg::ToggleDrive,
        KeyCode::Char('g') => Msg::ToggleGate,
        KeyCode::Enter => Msg::ToggleSelected,
        KeyCode::Char('0') => Msg::ResetParam,
        KeyCode::Char(c @ '1'..='9') => Msg::JumpStrip(c as usize - '1' as usize),
        KeyCode::Char('t') => Msg::SwapPanel,
        KeyCode::Char('r') => Msg::ResetStats,
        KeyCode::Char('?') => Msg::Help,
        KeyCode::Esc => Msg::CloseOverlay,
        KeyCode::Char('q') => Msg::Quit,
        _ => return None,
    })
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Row {
    pub strip: usize,
    pub param: usize,
}

pub struct App {
    pub strips: Vec<Strip>,
    pub device: Device,
    pub rows: Vec<Row>,
    pub selected: usize,
    pub values: Vec<Vec<f64>>,
    pub snapshot: Snapshot,
    pub in_meters: Vec<Meter>,
    pub out_meters: Vec<Meter>,
    pub in_clip: ClipLatch,
    pub out_clip: ClipLatch,
    pub stats: Stats,
    pub show_histogram: bool,
    pub help: bool,
    pub quit: bool,
    pub bypass_changed: Option<Instant>,
    pub now: Instant,
    pub bucket_upper_ns: Vec<u64>,
    message: Option<(String, Instant)>,
    quit_armed: Option<Instant>,
}

impl App {
    pub fn new(engine: &dyn Engine, now: Instant) -> Self {
        let strips = engine.strips().to_vec();
        let rows = strips
            .iter()
            .enumerate()
            .flat_map(|(s, strip)| {
                strip
                    .params
                    .iter()
                    .enumerate()
                    .filter(|(_, p)| p.is_row())
                    .map(move |(i, _)| Row { strip: s, param: i })
            })
            .collect();
        let channels = usize::try_from(engine.device().channels).unwrap_or(0);
        let snapshot = Snapshot::empty(&strips, channels);
        App {
            values: snapshot.params.clone(),
            device: engine.device().clone(),
            strips,
            rows,
            selected: 0,
            snapshot,
            in_meters: vec![Meter::default(); channels],
            out_meters: vec![Meter::default(); channels],
            in_clip: ClipLatch::default(),
            out_clip: ClipLatch::default(),
            stats: Stats::new(),
            show_histogram: false,
            help: false,
            quit: false,
            bypass_changed: None,
            now,
            bucket_upper_ns: (0..HIST_BUCKETS).map(|b| engine.bucket_upper_ns(b)).collect(),
            message: None,
            quit_armed: None,
        }
    }

    pub fn update(&mut self, msg: Msg, engine: &mut dyn Engine, now: Instant) -> Result<(), EngineError> {
        self.now = now;
        if msg != Msg::Quit {
            self.quit_armed = None;
        }
        match msg {
            Msg::Up => self.selected = self.selected.saturating_sub(1),
            Msg::Down => {
                if self.selected + 1 < self.rows.len() {
                    self.selected += 1;
                }
            }
            Msg::Coarse(dir) => self.nudge(engine, taper::COARSE * f64::from(dir))?,
            Msg::Fine(dir) => self.nudge(engine, taper::FINE * f64::from(dir))?,
            Msg::JumpStrip(s) => {
                if let Some(i) = self.rows.iter().position(|r| r.strip == s) {
                    self.selected = i;
                }
            }
            Msg::ToggleSelected => {
                if let Some(r) = self.rows.get(self.selected).copied() {
                    self.toggle_strip(engine, r.strip)?;
                }
            }
            Msg::ToggleBypass => self.toggle_strip(engine, 0)?,
            Msg::ToggleDrive => self.toggle_named(engine, "Drive")?,
            Msg::ToggleGate => self.toggle_named(engine, "Gate")?,
            Msg::ResetParam => {
                if let Some(r) = self.rows.get(self.selected).copied() {
                    let d = self.strips[r.strip].params[r.param].default;
                    self.set(engine, r.strip, r.param, d)?;
                }
            }
            Msg::ResetStats => self.stats.reset(),
            Msg::SwapPanel => self.show_histogram = !self.show_histogram,
            Msg::Help => self.help = !self.help,
            Msg::CloseOverlay => self.help = false,
            Msg::Quit => {
                if self.quit_armed.is_some_and(|t| now.saturating_duration_since(t) <= QUIT_WINDOW) {
                    self.quit = true;
                } else {
                    self.quit_armed = Some(now);
                    self.flash("press q again to quit");
                }
            }
            Msg::QuitNow => self.quit = true,
        }
        Ok(())
    }

    pub fn ingest(&mut self, snap: Snapshot, engine: &dyn Engine, now: Instant) {
        self.now = now;
        for (m, &p) in self.in_meters.iter_mut().zip(&snap.in_peak) {
            m.update(p, now);
        }
        for (m, &p) in self.out_meters.iter_mut().zip(&snap.out_peak) {
            m.update(p, now);
        }
        self.in_clip.update(snap.in_clips, now);
        self.out_clip.update(snap.out_clips, now);
        self.stats.ingest(&snap, now, &|h, p| engine.percentile_ns(h, p));
        self.values = snap.params.clone();
        self.snapshot = snap;
    }

    pub fn status(&self) -> Status {
        self.stats.status(self.now, self.device.block_ms())
    }

    pub fn message(&self) -> Option<&str> {
        self.message
            .as_ref()
            .filter(|(_, t)| self.now.saturating_duration_since(*t) < MESSAGE_TIME)
            .map(|(m, _)| m.as_str())
    }

    pub fn bypassed(&self) -> bool {
        !self.strip_on_value(0)
    }

    pub fn strip_on(&self, strip: usize) -> bool {
        if strip == 0 { true } else { self.strip_on_value(strip) }
    }

    // Master's switch is bypass, so its sense is inverted relative to the others.
    fn strip_on_value(&self, strip: usize) -> bool {
        match self.strips.get(strip).and_then(Strip::toggle_index) {
            Some(i) if strip == 0 => self.values[0][i] < 0.5,
            Some(i) => self.values[strip][i] >= 0.5,
            None => true,
        }
    }

    fn flash(&mut self, msg: &str) {
        self.message = Some((msg.to_owned(), self.now));
    }

    fn set(&mut self, engine: &mut dyn Engine, strip: usize, param: usize, v: f64) -> Result<(), EngineError> {
        match engine.set_param(strip, param, v) {
            Ok(()) => {
                self.values[strip][param] = v;
                Ok(())
            }
            Err(e @ (EngineError::Arg(_) | EngineError::State(_))) => {
                if cfg!(debug_assertions) {
                    panic!("set_param({strip}, {param}, {v}) rejected: {e}");
                }
                self.flash(&format!("internal: {e}"));
                Ok(())
            }
            Err(e) => Err(e),
        }
    }

    fn nudge(&mut self, engine: &mut dyn Engine, delta: f64) -> Result<(), EngineError> {
        let Some(r) = self.rows.get(self.selected).copied() else { return Ok(()) };
        let v = taper::step(&self.strips[r.strip].params[r.param], self.values[r.strip][r.param], delta);
        self.set(engine, r.strip, r.param, v)
    }

    fn toggle_strip(&mut self, engine: &mut dyn Engine, strip: usize) -> Result<(), EngineError> {
        let Some(i) = self.strips.get(strip).and_then(Strip::toggle_index) else { return Ok(()) };
        let next = if self.values[strip][i] >= 0.5 { 0.0 } else { 1.0 };
        self.set(engine, strip, i, next)?;
        if strip == 0 {
            self.bypass_changed = Some(self.now);
        }
        Ok(())
    }

    fn toggle_named(&mut self, engine: &mut dyn Engine, name: &str) -> Result<(), EngineError> {
        match self.strips.iter().position(|s| s.name == name) {
            Some(s) => self.toggle_strip(engine, s),
            None => Ok(()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::{EngineError, FakeEngine};
    use crate::taper;
    use ratatui::crossterm::event::{Event, KeyCode, KeyEvent, KeyEventKind, KeyModifiers};
    use std::time::{Duration, Instant};

    fn key(code: KeyCode) -> Event {
        Event::Key(KeyEvent::new(code, KeyModifiers::NONE))
    }

    fn setup() -> (FakeEngine, App, Instant) {
        let e = FakeEngine::rig();
        let t0 = Instant::now();
        let app = App::new(&e, t0);
        (e, app, t0)
    }

    #[test]
    fn release_events_are_ignored_and_repeats_accepted() {
        let release = Event::Key(KeyEvent::new_with_kind(
            KeyCode::Char(' '),
            KeyModifiers::NONE,
            KeyEventKind::Release,
        ));
        assert_eq!(map_key(&release), None);
        let repeat = Event::Key(KeyEvent::new_with_kind(
            KeyCode::Right,
            KeyModifiers::NONE,
            KeyEventKind::Repeat,
        ));
        assert_eq!(map_key(&repeat), Some(Msg::Coarse(1)));
        assert_eq!(map_key(&Event::Resize(80, 24)), None);
    }

    #[test]
    fn every_binding() {
        use KeyCode::*;
        let cases = [
            (Up, Msg::Up),
            (Char('k'), Msg::Up),
            (Down, Msg::Down),
            (Char('j'), Msg::Down),
            (Left, Msg::Coarse(-1)),
            (Char('h'), Msg::Coarse(-1)),
            (Right, Msg::Coarse(1)),
            (Char('l'), Msg::Coarse(1)),
            (Char('['), Msg::Fine(-1)),
            (Char(']'), Msg::Fine(1)),
            (Char(' '), Msg::ToggleBypass),
            (Char('d'), Msg::ToggleDrive),
            (Char('g'), Msg::ToggleGate),
            (Enter, Msg::ToggleSelected),
            (Char('0'), Msg::ResetParam),
            (Char('1'), Msg::JumpStrip(0)),
            (Char('5'), Msg::JumpStrip(4)),
            (Char('9'), Msg::JumpStrip(8)),
            (Char('t'), Msg::SwapPanel),
            (Char('r'), Msg::ResetStats),
            (Char('?'), Msg::Help),
            (Esc, Msg::CloseOverlay),
            (Char('q'), Msg::Quit),
        ];
        for (code, msg) in cases {
            assert_eq!(map_key(&key(code)), Some(msg), "{code:?}");
        }
        assert_eq!(map_key(&key(Char('x'))), None);
        let ctrl_c = Event::Key(KeyEvent::new(Char('c'), KeyModifiers::CONTROL));
        assert_eq!(map_key(&ctrl_c), Some(Msg::QuitNow));
        let ctrl_q = Event::Key(KeyEvent::new(Char('q'), KeyModifiers::CONTROL));
        assert_eq!(map_key(&ctrl_q), None);
    }

    #[test]
    fn rows_skip_toggles_and_read_only_params() {
        let (_, app, _) = setup();
        let rows: Vec<(usize, usize)> = app.rows.iter().map(|r| (r.strip, r.param)).collect();
        assert_eq!(
            rows,
            [(0, 0), (0, 1), (1, 0), (1, 1), (2, 1), (3, 1), (3, 2), (4, 0), (4, 1), (4, 2)]
        );
    }

    #[test]
    fn selection_moves_and_clamps() {
        let (mut e, mut app, t0) = setup();
        app.update(Msg::Up, &mut e, t0).unwrap();
        assert_eq!(app.selected, 0);
        for _ in 0..20 {
            app.update(Msg::Down, &mut e, t0).unwrap();
        }
        assert_eq!(app.selected, 9);
        app.update(Msg::JumpStrip(3), &mut e, t0).unwrap();
        assert_eq!(app.rows[app.selected], Row { strip: 3, param: 1 });
        app.update(Msg::JumpStrip(8), &mut e, t0).unwrap();
        assert_eq!(app.rows[app.selected], Row { strip: 3, param: 1 });
    }

    #[test]
    fn nudges_accumulate_before_the_next_snapshot() {
        let (mut e, mut app, t0) = setup();
        app.update(Msg::JumpStrip(3), &mut e, t0).unwrap();
        app.update(Msg::Coarse(1), &mut e, t0).unwrap();
        app.update(Msg::Coarse(1), &mut e, t0).unwrap();
        let p = &e.strips[3].params[1];
        let expected = taper::step(p, taper::step(p, 1.0, taper::COARSE), taper::COARSE);
        assert!((e.values[3][1] - expected).abs() < 1e-12);
        app.update(Msg::Fine(-1), &mut e, t0).unwrap();
        assert!(e.values[3][1] < expected);
    }

    #[test]
    fn toggles_flip_without_waiting_for_a_snapshot() {
        let (mut e, mut app, t0) = setup();
        app.update(Msg::ToggleBypass, &mut e, t0).unwrap();
        assert_eq!(e.values[0][2], 1.0);
        assert!(app.bypassed());
        assert_eq!(app.bypass_changed, Some(t0));
        app.update(Msg::ToggleBypass, &mut e, t0).unwrap();
        assert_eq!(e.values[0][2], 0.0);
        app.update(Msg::ToggleDrive, &mut e, t0).unwrap();
        assert_eq!(e.values[3][0], 0.0);
        assert!(!app.strip_on(3));
        app.update(Msg::ToggleGate, &mut e, t0).unwrap();
        assert_eq!(e.values[2][0], 0.0);
        app.update(Msg::JumpStrip(0), &mut e, t0).unwrap();
        app.update(Msg::ToggleSelected, &mut e, t0).unwrap();
        assert_eq!(e.values[0][2], 1.0);
        app.update(Msg::JumpStrip(1), &mut e, t0).unwrap();
        let before = e.sets.len();
        app.update(Msg::ToggleSelected, &mut e, t0).unwrap(); // high-pass has no switch
        assert_eq!(e.sets.len(), before);
    }

    #[test]
    fn reset_returns_to_the_instance_default() {
        let (mut e, mut app, t0) = setup();
        app.update(Msg::JumpStrip(4), &mut e, t0).unwrap();
        app.update(Msg::Down, &mut e, t0).unwrap();
        app.update(Msg::Down, &mut e, t0).unwrap(); // low shelf gain
        app.update(Msg::Coarse(1), &mut e, t0).unwrap();
        assert!(e.values[4][2] > 2.0);
        app.update(Msg::ResetParam, &mut e, t0).unwrap();
        assert_eq!(e.values[4][2], 2.0);
    }

    #[test]
    fn quit_needs_two_presses_close_together() {
        let (mut e, mut app, t0) = setup();
        app.update(Msg::Quit, &mut e, t0).unwrap();
        assert!(!app.quit);
        assert_eq!(app.message(), Some("press q again to quit"));
        app.update(Msg::Quit, &mut e, t0 + Duration::from_secs(1)).unwrap();
        assert!(app.quit);

        let (mut e, mut app, t0) = setup();
        app.update(Msg::Quit, &mut e, t0).unwrap();
        app.update(Msg::Quit, &mut e, t0 + Duration::from_secs(2)).unwrap();
        assert!(!app.quit, "the second press re-arms");
        app.update(Msg::Down, &mut e, t0 + Duration::from_millis(2100)).unwrap();
        app.update(Msg::Quit, &mut e, t0 + Duration::from_millis(2200)).unwrap();
        assert!(!app.quit, "another key disarms");

        let (mut e, mut app, t0) = setup();
        app.update(Msg::QuitNow, &mut e, t0).unwrap();
        assert!(app.quit);
    }

    #[test]
    fn internal_errors_propagate() {
        let (mut e, mut app, t0) = setup();
        e.fail_next_set = Some(EngineError::Internal("boom".into()));
        assert!(app.update(Msg::ToggleBypass, &mut e, t0).is_err());
    }

    #[test]
    fn ingest_takes_the_engine_values() {
        let (mut e, mut app, t0) = setup();
        e.values[3][1] = 20.0;
        e.next.in_peak = vec![0.5, 0.25];
        let snap = e.snapshot().unwrap();
        app.ingest(snap, &e, t0);
        assert_eq!(app.values[3][1], 20.0);
        assert!((app.in_meters[0].level_db() - crate::meters::to_db(0.5)).abs() < 1e-12);
        assert_eq!(app.status(), crate::stats::Status::Live);
    }

    #[test]
    fn help_and_panel_toggles() {
        let (mut e, mut app, t0) = setup();
        app.update(Msg::Help, &mut e, t0).unwrap();
        assert!(app.help);
        app.update(Msg::CloseOverlay, &mut e, t0).unwrap();
        assert!(!app.help);
        app.update(Msg::SwapPanel, &mut e, t0).unwrap();
        assert!(app.show_histogram);
    }
}
