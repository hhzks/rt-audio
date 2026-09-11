use ratatui::style::{Color, Modifier, Style};
use ratatui::widgets::BorderType;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GlyphChoice {
    Auto,
    Rich,
    Basic,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ColorChoice {
    Auto,
    TrueColor,
    Sixteen,
    None,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ColorMode {
    TrueColor,
    Sixteen,
    None,
}

pub struct Env {
    pub term: Option<String>,
    pub colorterm: Option<String>,
    pub wt_session: bool,
    pub no_color: bool,
}

impl Env {
    pub fn from_process() -> Self {
        Env {
            term: std::env::var("TERM").ok(),
            colorterm: std::env::var("COLORTERM").ok(),
            wt_session: std::env::var_os("WT_SESSION").is_some(),
            no_color: std::env::var_os("NO_COLOR").is_some_and(|v| !v.is_empty()),
        }
    }
}

pub fn pick_rich(choice: GlyphChoice, env: &Env) -> bool {
    match choice {
        GlyphChoice::Rich => true,
        GlyphChoice::Basic => false,
        GlyphChoice::Auto => env.term.as_deref() != Some("linux"),
    }
}

pub fn pick_colors(choice: ColorChoice, env: &Env) -> ColorMode {
    match choice {
        ColorChoice::TrueColor => ColorMode::TrueColor,
        ColorChoice::Sixteen => ColorMode::Sixteen,
        ColorChoice::None => ColorMode::None,
        ColorChoice::Auto if env.no_color => ColorMode::None,
        ColorChoice::Auto
            if matches!(env.colorterm.as_deref(), Some("truecolor" | "24bit"))
                || env.wt_session =>
        {
            ColorMode::TrueColor
        }
        ColorChoice::Auto => ColorMode::Sixteen,
    }
}

pub struct Glyphs {
    pub border: BorderType,
    pub vbar: &'static [&'static str],
    pub hbar: &'static [&'static str],
    pub slider_fill: &'static str,
    pub slider_empty: &'static str,
    pub slider_knob: &'static str,
    pub hold: &'static str,
    pub led_on: &'static str,
    pub led_off: &'static str,
    pub cursor: &'static str,
    pub live: &'static str,
    pub stalled: &'static str,
    pub stopped: &'static str,
    pub xrun: &'static str,
    pub deadline: &'static str,
    pub key_space: &'static str,
    pub key_updown: &'static str,
    pub key_leftright: &'static str,
    pub times: &'static str,
    pub minus: &'static str,
    pub effects: bool,
}

impl Glyphs {
    pub fn rich() -> Self {
        Glyphs {
            border: BorderType::Rounded,
            vbar: &[" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"],
            hbar: &[" ", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█"],
            slider_fill: "━",
            slider_empty: "─",
            slider_knob: "●",
            hold: "▔",
            led_on: "●",
            led_off: "○",
            cursor: "▸",
            live: "▶",
            stalled: "‖",
            stopped: "■",
            xrun: "▲",
            deadline: "┊",
            key_space: "␣",
            key_updown: "↑↓",
            key_leftright: "←→",
            times: "×",
            minus: "−",
            effects: true,
        }
    }

    pub fn basic() -> Self {
        Glyphs {
            border: BorderType::Plain,
            vbar: &[" ", "▄", "█"],
            hbar: &[" ", "▌", "█"],
            slider_fill: "=",
            slider_empty: "-",
            slider_knob: "O",
            hold: "-",
            led_on: "*",
            led_off: "o",
            cursor: ">",
            live: ">",
            stalled: "!",
            stopped: "#",
            xrun: "^",
            deadline: ":",
            key_space: "spc",
            key_updown: "u/d",
            key_leftright: "l/r",
            times: "x",
            minus: "-",
            effects: false,
        }
    }

    fn cell(set: &'static [&'static str], fill: f64) -> &'static str {
        let steps = set.len() - 1;
        let i = (fill.clamp(0.0, 1.0) * steps as f64).round() as usize;
        set[i.min(steps)]
    }

    pub fn vbar_cell(&self, fill: f64) -> &'static str {
        Self::cell(self.vbar, fill)
    }

    pub fn hbar_cell(&self, fill: f64) -> &'static str {
        Self::cell(self.hbar, fill)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Role {
    Normal,
    Dim,
    Title,
    Accent,
    Good,
    Warn,
    Bad,
    Selected,
}

pub fn zone(db: f64) -> Role {
    if db > -6.0 {
        Role::Bad
    } else if db > -18.0 {
        Role::Warn
    } else {
        Role::Good
    }
}

pub struct Theme {
    pub rich: bool,
    pub glyphs: Glyphs,
    pub colors: ColorMode,
}

impl Theme {
    pub fn new(rich: bool, colors: ColorMode) -> Self {
        Theme {
            rich,
            glyphs: if rich {
                Glyphs::rich()
            } else {
                Glyphs::basic()
            },
            colors,
        }
    }

    fn color(&self, role: Role) -> Option<Color> {
        let (rgb, named) = match role {
            Role::Normal | Role::Selected => return None,
            Role::Dim => ((110, 110, 120), Color::DarkGray),
            Role::Title => ((200, 200, 215), Color::White),
            Role::Accent => ((90, 170, 255), Color::Cyan),
            Role::Good => ((80, 200, 120), Color::Green),
            Role::Warn => ((230, 180, 60), Color::Yellow),
            Role::Bad => ((230, 70, 70), Color::Red),
        };
        match self.colors {
            ColorMode::TrueColor => Some(Color::Rgb(rgb.0, rgb.1, rgb.2)),
            ColorMode::Sixteen => Some(named),
            ColorMode::None => None,
        }
    }

    pub fn style(&self, role: Role) -> Style {
        let mut s = Style::new();
        if let Some(c) = self.color(role) {
            s = s.fg(c);
        }
        match role {
            Role::Selected => s.add_modifier(Modifier::REVERSED),
            Role::Dim => s.add_modifier(Modifier::DIM),
            Role::Bad | Role::Title => s.add_modifier(Modifier::BOLD),
            _ => s,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use ratatui::style::Modifier;

    fn env(term: Option<&str>, colorterm: Option<&str>, wt: bool, no_color: bool) -> Env {
        Env {
            term: term.map(Into::into),
            colorterm: colorterm.map(Into::into),
            wt_session: wt,
            no_color,
        }
    }

    const CP437_SAFE: &str = "─│┌┐└┘├┤┬┴┼█▄▀▌▐░▒▓";

    fn basic_safe(s: &str) -> bool {
        s.chars().all(|c| c.is_ascii() || CP437_SAFE.contains(c))
    }

    #[test]
    fn linux_console_gets_basic_glyphs() {
        assert!(!pick_rich(
            GlyphChoice::Auto,
            &env(Some("linux"), None, false, false)
        ));
        assert!(pick_rich(
            GlyphChoice::Auto,
            &env(Some("xterm-256color"), None, false, false)
        ));
        assert!(pick_rich(GlyphChoice::Auto, &env(None, None, false, false)));
        assert!(!pick_rich(
            GlyphChoice::Basic,
            &env(None, None, false, false)
        ));
        assert!(pick_rich(
            GlyphChoice::Rich,
            &env(Some("linux"), None, false, false)
        ));
    }

    #[test]
    fn colour_detection() {
        use ColorMode as M;
        let auto = ColorChoice::Auto;
        assert_eq!(
            pick_colors(auto, &env(None, Some("truecolor"), false, true)),
            M::None
        );
        assert_eq!(
            pick_colors(auto, &env(None, Some("truecolor"), false, false)),
            M::TrueColor
        );
        assert_eq!(
            pick_colors(auto, &env(None, Some("24bit"), false, false)),
            M::TrueColor
        );
        assert_eq!(
            pick_colors(auto, &env(None, None, true, false)),
            M::TrueColor
        );
        assert_eq!(
            pick_colors(auto, &env(Some("xterm"), None, false, false)),
            M::Sixteen
        );
        assert_eq!(
            pick_colors(auto, &env(Some("linux"), None, false, false)),
            M::Sixteen
        );
        assert_eq!(
            pick_colors(
                ColorChoice::None,
                &env(None, Some("truecolor"), false, false)
            ),
            M::None
        );
        assert_eq!(
            pick_colors(ColorChoice::TrueColor, &env(None, None, false, true)),
            M::TrueColor
        );
    }

    #[test]
    fn basic_glyphs_stay_in_the_console_font() {
        let g = Glyphs::basic();
        let all = [
            g.slider_fill,
            g.slider_empty,
            g.slider_knob,
            g.hold,
            g.led_on,
            g.led_off,
            g.cursor,
            g.live,
            g.stalled,
            g.stopped,
            g.xrun,
            g.deadline,
            g.key_space,
            g.key_updown,
            g.key_leftright,
            g.times,
            g.minus,
        ];
        for s in all.iter().chain(g.vbar.iter()).chain(g.hbar.iter()) {
            assert!(basic_safe(s), "{s:?}");
        }
        assert!(!g.effects);
        assert!(Glyphs::rich().effects);
    }

    #[test]
    fn bar_cells() {
        let rich = Glyphs::rich();
        let basic = Glyphs::basic();
        assert_eq!(rich.vbar_cell(0.0), " ");
        assert_eq!(rich.vbar_cell(1.0), "█");
        assert_eq!(rich.vbar_cell(0.5), "▄");
        assert_eq!(rich.vbar_cell(2.0), "█");
        assert_eq!(rich.vbar_cell(-1.0), " ");
        assert_eq!(basic.vbar_cell(0.5), "▄");
        assert_eq!(basic.vbar_cell(0.2), " ");
        assert_eq!(rich.hbar_cell(0.25), "▎");
        assert_eq!(basic.hbar_cell(0.5), "▌");
    }

    #[test]
    fn zones() {
        assert_eq!(zone(-30.0), Role::Good);
        assert_eq!(zone(-12.0), Role::Warn);
        assert_eq!(zone(-3.0), Role::Bad);
    }

    #[test]
    fn no_colour_mode_still_marks_state() {
        let t = Theme::new(true, ColorMode::None);
        assert!(t.style(Role::Good).fg.is_none());
        assert!(
            t.style(Role::Selected)
                .add_modifier
                .contains(Modifier::REVERSED)
        );
        assert!(t.style(Role::Dim).add_modifier.contains(Modifier::DIM));
        assert!(t.style(Role::Bad).add_modifier.contains(Modifier::BOLD));
        let c = Theme::new(true, ColorMode::TrueColor);
        assert!(c.style(Role::Good).fg.is_some());
        assert!(
            c.style(Role::Selected)
                .add_modifier
                .contains(Modifier::REVERSED)
        );
    }
}
