use std::time::{Duration, Instant};

pub const FLOOR_DB: f64 = -60.0;
pub const RELEASE_DB_PER_S: f64 = 12.0;
pub const HOLD: Duration = Duration::from_millis(1500);
pub const CLIP_LATCH: Duration = Duration::from_secs(2);

pub fn to_db(peak: f32) -> f64 {
    if peak <= 0.0 {
        FLOOR_DB
    } else {
        (20.0 * f64::from(peak).log10()).max(FLOOR_DB)
    }
}

#[derive(Clone, Debug)]
pub struct Meter {
    level_db: f64,
    hold_db: f64,
    hold_until: Option<Instant>,
    last: Option<Instant>,
}

impl Default for Meter {
    fn default() -> Self {
        Meter { level_db: FLOOR_DB, hold_db: FLOOR_DB, hold_until: None, last: None }
    }
}

impl Meter {
    pub fn update(&mut self, peak: f32, now: Instant) {
        let input = to_db(peak);
        let dt = self.last.map_or(0.0, |t| now.saturating_duration_since(t).as_secs_f64());
        self.last = Some(now);

        let fallen = (self.level_db - RELEASE_DB_PER_S * dt).max(FLOOR_DB);
        self.level_db = input.max(fallen);

        if input >= self.hold_db {
            self.hold_db = input;
            self.hold_until = Some(now + HOLD);
        } else if self.hold_until.is_some_and(|t| now >= t) {
            self.hold_db = (self.hold_db - RELEASE_DB_PER_S * dt).max(self.level_db);
        }
    }

    pub fn level_db(&self) -> f64 {
        self.level_db
    }

    pub fn hold_db(&self) -> f64 {
        self.hold_db
    }
}

#[derive(Clone, Debug, Default)]
pub struct ClipLatch {
    last_count: Option<u64>,
    lit_until: Option<Instant>,
}

impl ClipLatch {
    pub fn update(&mut self, count: u64, now: Instant) {
        if self.last_count.is_some_and(|last| count > last) {
            self.lit_until = Some(now + CLIP_LATCH);
        }
        self.last_count = Some(count);
    }

    pub fn lit(&self, now: Instant) -> bool {
        self.lit_until.is_some_and(|t| now < t)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{Duration, Instant};

    fn at(t0: Instant, secs: f64) -> Instant {
        t0 + Duration::from_secs_f64(secs)
    }

    fn level_after_one_second(fps: u32) -> f64 {
        let t0 = Instant::now();
        let mut m = Meter::default();
        m.update(1.0, t0);
        for i in 1..=fps {
            m.update(0.0, at(t0, f64::from(i) / f64::from(fps)));
        }
        m.level_db()
    }

    #[test]
    fn to_db_has_a_floor() {
        assert_eq!(to_db(0.0), FLOOR_DB);
        assert_eq!(to_db(1e-9), FLOOR_DB);
        assert!((to_db(1.0) - 0.0).abs() < 1e-12);
        assert!((to_db(0.5) - -6.0206).abs() < 1e-3);
    }

    #[test]
    fn release_does_not_depend_on_frame_rate() {
        let a = level_after_one_second(30);
        let b = level_after_one_second(15);
        assert!((a - -12.0).abs() < 1e-9, "{a}");
        assert!((a - b).abs() < 1e-9);
    }

    #[test]
    fn attack_is_instant() {
        let t0 = Instant::now();
        let mut m = Meter::default();
        m.update(0.0, t0);
        m.update(0.5, at(t0, 0.033));
        assert!((m.level_db() - to_db(0.5)).abs() < 1e-12);
    }

    #[test]
    fn peak_hold_lasts_then_falls() {
        let t0 = Instant::now();
        let mut m = Meter::default();
        m.update(1.0, t0);
        let mut t = 0.0;
        while t < 1.4 {
            t += 0.1;
            m.update(0.0, at(t0, t));
        }
        assert_eq!(m.hold_db(), 0.0);
        while t < 2.0 {
            t += 0.1;
            m.update(0.0, at(t0, t));
        }
        assert!(m.hold_db() < -3.0);
        assert!(m.hold_db() >= m.level_db());
    }

    #[test]
    fn clip_latch_lights_for_two_seconds() {
        let t0 = Instant::now();
        let mut c = ClipLatch::default();
        c.update(5, t0);
        assert!(!c.lit(t0), "the first update only records");
        c.update(6, at(t0, 0.1));
        assert!(c.lit(at(t0, 0.1)));
        assert!(c.lit(at(t0, 2.0)));
        assert!(!c.lit(at(t0, 2.2)));
        c.update(6, at(t0, 2.3));
        assert!(!c.lit(at(t0, 2.3)));
    }
}
