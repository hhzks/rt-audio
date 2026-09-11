use crate::model::{Param, Taper};

pub const COARSE: f64 = 0.05;
pub const FINE: f64 = 0.005;

pub fn to_position(p: &Param, v: f64) -> f64 {
    let v = v.clamp(p.min, p.max);
    match p.taper {
        Taper::Linear => (v - p.min) / (p.max - p.min),
        Taper::Log => (v / p.min).ln() / (p.max / p.min).ln(),
    }
}

pub fn from_position(p: &Param, pos: f64) -> f64 {
    let pos = pos.clamp(0.0, 1.0);
    match p.taper {
        Taper::Linear => p.min + pos * (p.max - p.min),
        Taper::Log => p.min * (p.max / p.min).powf(pos),
    }
}

pub fn step(p: &Param, v: f64, delta: f64) -> f64 {
    from_position(p, to_position(p, v) + delta)
}

pub fn format_value(p: &Param, v: f64, rich: bool) -> String {
    let s = match p.unit.as_str() {
        "Hz" if v >= 1000.0 => format!("{:.1} kHz", v / 1000.0),
        "Hz" => format!("{v:.0} Hz"),
        "dB" => format!("{v:+.1} dB"),
        "%" => format!("{:.0} %", v * 100.0),
        "x" => format!("{v:.1} {}", if rich { '\u{00d7}' } else { 'x' }),
        "" => format!("{v:.2}"),
        other => format!("{v:.2} {other}"),
    };
    if rich { s.replace('-', "\u{2212}") } else { s }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::FakeEngine;

    fn param(strip: usize, id: &str) -> crate::model::Param {
        let e = FakeEngine::rig();
        let s = &e.strips[strip];
        s.params[s.param_index(id).unwrap()].clone()
    }

    #[test]
    fn linear_positions() {
        let out = param(0, "out_gain");
        assert_eq!(to_position(&out, -60.0), 0.0);
        assert_eq!(to_position(&out, 12.0), 1.0);
        assert!((from_position(&out, 0.5) - -24.0).abs() < 1e-12);
    }

    #[test]
    fn log_midpoint_is_the_geometric_mean() {
        let f = param(1, "freq");
        let mid = (20.0_f64 * 20000.0).sqrt();
        assert!((to_position(&f, mid) - 0.5).abs() < 1e-12);
        assert!((from_position(&f, 0.5) - mid).abs() < 1e-9);
    }

    #[test]
    fn round_trips() {
        for (strip, id) in [
            (1, "freq"),
            (1, "q"),
            (3, "drive"),
            (3, "mix"),
            (0, "in_gain"),
        ] {
            let p = param(strip, id);
            for i in 0..=20 {
                let pos = f64::from(i) / 20.0;
                let back = to_position(&p, from_position(&p, pos));
                assert!((back - pos).abs() < 1e-9, "{id} at {pos}");
            }
        }
    }

    #[test]
    fn log_steps_change_frequency_by_the_same_ratio() {
        let f = param(1, "freq");
        let low = step(&f, 80.0, COARSE) / 80.0;
        let high = step(&f, 8000.0, COARSE) / 8000.0;
        assert!((low - high).abs() < 1e-9);
        assert!(low > 1.0);
    }

    #[test]
    fn steps_clamp_at_the_ends() {
        let f = param(1, "freq");
        assert_eq!(step(&f, 20000.0, COARSE), 20000.0);
        assert_eq!(step(&f, 20.0, -COARSE), 20.0);
    }

    #[test]
    fn formatting() {
        assert_eq!(format_value(&param(1, "freq"), 80.0, true), "80 Hz");
        assert_eq!(format_value(&param(1, "freq"), 1500.0, true), "1.5 kHz");
        assert_eq!(format_value(&param(1, "q"), 0.707, true), "0.71");
        assert_eq!(format_value(&param(0, "out_gain"), -3.0, false), "-3.0 dB");
        assert_eq!(
            format_value(&param(0, "out_gain"), -3.0, true),
            "\u{2212}3.0 dB"
        );
        assert_eq!(format_value(&param(0, "in_gain"), 0.0, true), "+0.0 dB");
        assert_eq!(format_value(&param(3, "mix"), 0.9, true), "90 %");
        assert_eq!(format_value(&param(3, "drive"), 6.0, true), "6.0 \u{00d7}");
        assert_eq!(format_value(&param(3, "drive"), 6.0, false), "6.0 x");
    }
}
