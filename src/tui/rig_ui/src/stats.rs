use std::collections::VecDeque;
use std::time::{Duration, Instant};

use crate::model::{HIST_BUCKETS, Histogram, Snapshot};

pub const LOAD_HISTORY: usize = 60;
pub const XRUN_FLASH: Duration = Duration::from_secs(2);
const STALL_MIN: Duration = Duration::from_millis(500);

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Status {
    Live,
    Stalled(Duration),
    Stopped,
}

pub struct Stats {
    run: Box<Histogram>,
    second: Box<Histogram>,
    second_started: Option<Instant>,
    pub load_history: VecDeque<f64>,
    pub last_load: f64,
    pub run_p50_ns: u64,
    pub run_p99_ns: u64,
    pub run_p999_ns: u64,
    pub run_max_ns: u64,
    pub deadline_ns: u64,
    dropouts_total: u64,
    dropouts_base: u64,
    xrun_flash_until: Option<Instant>,
    last_callbacks: Option<u64>,
    last_progress: Option<Instant>,
    running: bool,
}

impl Default for Stats {
    fn default() -> Self {
        Self::new()
    }
}

impl Stats {
    pub fn new() -> Self {
        Stats {
            run: Box::new([0; HIST_BUCKETS]),
            second: Box::new([0; HIST_BUCKETS]),
            second_started: None,
            load_history: VecDeque::with_capacity(LOAD_HISTORY),
            last_load: 0.0,
            run_p50_ns: 0,
            run_p99_ns: 0,
            run_p999_ns: 0,
            run_max_ns: 0,
            deadline_ns: 0,
            dropouts_total: 0,
            dropouts_base: 0,
            xrun_flash_until: None,
            last_callbacks: None,
            last_progress: None,
            running: true,
        }
    }

    pub fn ingest(&mut self, snap: &Snapshot, now: Instant, pct: &dyn Fn(&Histogram, f64) -> u64) {
        self.deadline_ns = snap.deadline_ns;
        self.running = snap.running;

        for (r, w) in self.run.iter_mut().zip(snap.hist_window.iter()) {
            *r += w;
        }
        for (s, w) in self.second.iter_mut().zip(snap.hist_window.iter()) {
            *s += w;
        }

        let started = *self.second_started.get_or_insert(now);
        if now.saturating_duration_since(started) >= Duration::from_secs(1) {
            self.last_load = self.ratio(pct(&self.second, 0.99));
            self.load_history.push_back(self.last_load);
            while self.load_history.len() > LOAD_HISTORY {
                self.load_history.pop_front();
            }
            self.second.fill(0);
            self.second_started = Some(now);
        }

        self.run_p50_ns = pct(&self.run, 0.5);
        self.run_p99_ns = pct(&self.run, 0.99);
        self.run_p999_ns = pct(&self.run, 0.999);
        self.run_max_ns = pct(&self.run, 1.0);

        let total = snap.dropouts();
        if self.last_callbacks.is_some() && total > self.dropouts_total {
            self.xrun_flash_until = Some(now + XRUN_FLASH);
        }
        self.dropouts_total = total;

        if self.last_callbacks != Some(snap.callbacks) {
            self.last_callbacks = Some(snap.callbacks);
            self.last_progress = Some(now);
        }
    }

    pub fn ratio(&self, ns: u64) -> f64 {
        if self.deadline_ns == 0 { 0.0 } else { ns as f64 / self.deadline_ns as f64 }
    }

    pub fn dropouts(&self) -> u64 {
        self.dropouts_total.saturating_sub(self.dropouts_base)
    }

    pub fn xrun_flash(&self, now: Instant) -> bool {
        self.xrun_flash_until.is_some_and(|t| now < t)
    }

    pub fn run_histogram(&self) -> &Histogram {
        &self.run
    }

    pub fn reset(&mut self) {
        self.run.fill(0);
        self.second.fill(0);
        self.second_started = None;
        self.load_history.clear();
        self.last_load = 0.0;
        self.run_p50_ns = 0;
        self.run_p99_ns = 0;
        self.run_p999_ns = 0;
        self.run_max_ns = 0;
        self.dropouts_base = self.dropouts_total;
        self.xrun_flash_until = None;
    }

    pub fn status(&self, now: Instant, block_ms: f64) -> Status {
        if !self.running {
            return Status::Stopped;
        }
        let threshold = STALL_MIN.max(Duration::from_secs_f64((4.0 * block_ms / 1000.0).max(0.0)));
        match self.last_progress {
            Some(t) => {
                let quiet = now.saturating_duration_since(t);
                if quiet >= threshold { Status::Stalled(quiet) } else { Status::Live }
            }
            None => Status::Live,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::{Engine, FakeEngine, Snapshot};
    use std::time::{Duration, Instant};

    fn at(t0: Instant, secs: f64) -> Instant {
        t0 + Duration::from_secs_f64(secs)
    }

    fn snap(callbacks: u64, dropouts: u64) -> Snapshot {
        let mut s = Snapshot::empty(&FakeEngine::rig().strips, 2);
        s.callbacks = callbacks;
        s.engine_xruns = dropouts;
        s.deadline_ns = 1_000_000;
        s.hist_window[99] = 10; // FakeEngine: bucket 99 upper edge = 100 µs
        s
    }

    fn feed(stats: &mut Stats, s: &Snapshot, now: Instant) {
        let e = FakeEngine::rig();
        stats.ingest(s, now, &|h, p| e.percentile_ns(h, p));
    }

    #[test]
    fn load_rolls_over_each_second() {
        let t0 = Instant::now();
        let mut st = Stats::new();
        feed(&mut st, &snap(1, 0), t0);
        feed(&mut st, &snap(2, 0), at(t0, 0.5));
        assert!(st.load_history.is_empty());
        feed(&mut st, &snap(3, 0), at(t0, 1.0));
        assert_eq!(st.load_history.len(), 1);
        assert!((st.last_load - 0.1).abs() < 1e-12);
    }

    #[test]
    fn load_history_is_capped() {
        let t0 = Instant::now();
        let mut st = Stats::new();
        for i in 0..=(LOAD_HISTORY as u64 + 10) {
            feed(&mut st, &snap(i, 0), at(t0, i as f64));
        }
        assert_eq!(st.load_history.len(), LOAD_HISTORY);
    }

    #[test]
    fn run_percentiles_accumulate_and_reset() {
        let t0 = Instant::now();
        let mut st = Stats::new();
        feed(&mut st, &snap(1, 0), t0);
        assert_eq!(st.run_p99_ns, 100_000);
        assert_eq!(st.run_max_ns, 100_000);
        assert!((st.ratio(st.run_p99_ns) - 0.1).abs() < 1e-12);
        st.reset();
        assert_eq!(st.run_p99_ns, 0);
        assert!(st.run_histogram().iter().all(|&c| c == 0));
    }

    #[test]
    fn dropouts_use_a_baseline_and_flash() {
        let t0 = Instant::now();
        let mut st = Stats::new();
        feed(&mut st, &snap(1, 0), t0);
        assert!(!st.xrun_flash(t0));
        feed(&mut st, &snap(2, 2), at(t0, 0.1));
        assert_eq!(st.dropouts(), 2);
        assert!(st.xrun_flash(at(t0, 1.0)));
        assert!(!st.xrun_flash(at(t0, 2.2)));
        st.reset();
        assert_eq!(st.dropouts(), 0);
        feed(&mut st, &snap(3, 3), at(t0, 3.0));
        assert_eq!(st.dropouts(), 1);
    }

    #[test]
    fn first_snapshot_does_not_flash() {
        let t0 = Instant::now();
        let mut st = Stats::new();
        feed(&mut st, &snap(1, 7), t0);
        assert!(!st.xrun_flash(t0));
        assert_eq!(st.dropouts(), 7);
    }

    #[test]
    fn watchdog_states() {
        let t0 = Instant::now();
        let mut st = Stats::new();
        feed(&mut st, &snap(10, 0), t0);
        feed(&mut st, &snap(10, 0), at(t0, 0.4));
        assert_eq!(st.status(at(t0, 0.4), 2.67), Status::Live);
        assert!(matches!(st.status(at(t0, 0.6), 2.67), Status::Stalled(d) if d > Duration::from_millis(590)));
        assert_eq!(st.status(at(t0, 0.6), 200.0), Status::Live); // threshold is 4 x 200 ms
        feed(&mut st, &snap(11, 0), at(t0, 0.7));
        assert_eq!(st.status(at(t0, 0.7), 2.67), Status::Live);
        let mut stopped = snap(12, 0);
        stopped.running = false;
        feed(&mut st, &stopped, at(t0, 0.8));
        assert_eq!(st.status(at(t0, 0.8), 2.67), Status::Stopped);
    }
}
