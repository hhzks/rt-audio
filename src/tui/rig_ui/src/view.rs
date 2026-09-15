use std::time::{Duration, Instant};

use ratatui::Frame;
use ratatui::buffer::Buffer;
use ratatui::layout::{Constraint, Layout, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::widgets::{Block, Borders, Clear, Widget};
use tachyonfx::{EffectManager, Motion, fx};

use crate::app::{App, Mode, Row};
use crate::latency::Step;
use crate::meters::{FLOOR_DB, Meter};
use crate::model::{LatencyKind, LatencyPhase, LatencyState};
use crate::picker::{Field, PickerStatus, khz};
use crate::stats::Status;
use crate::taper;
use crate::theme::{Role, Theme, zone};

pub const MIN_W: u16 = 80;
pub const MIN_H: u16 = 24;
pub const WIDE_W: u16 = 120;
const METER_W: u16 = 10;
const HIST_W: u16 = 40;
const SLIDER_W: usize = 16;

const HELP: [(&str, &str); 14] = [
    ("space", "bypass the whole chain"),
    ("d / g", "drive / gate on or off"),
    ("enter", "selected strip on or off"),
    ("up dn", "select a parameter (j k)"),
    ("lt rt", "adjust by 5% (h l)"),
    ("[ ]", "adjust by 0.5%"),
    ("1-9", "jump to a strip"),
    ("0", "reset to default"),
    ("t", "swap chain and histogram"),
    ("r", "reset stats"),
    ("o", "devices and stream settings"),
    ("m", "measure latency"),
    ("q q", "quit"),
    ("ctrl+c", "quit now"),
];

pub fn render(app: &App, theme: &Theme, f: &mut Frame) {
    let area = f.area();
    draw(app, theme, area, f.buffer_mut());
}

pub fn draw(app: &App, theme: &Theme, area: Rect, buf: &mut Buffer) {
    if area.width < MIN_W || area.height < MIN_H {
        too_small(theme, area, buf);
        return;
    }
    let [header, body, engine, hints] = Layout::vertical([
        Constraint::Length(1),
        Constraint::Fill(1),
        Constraint::Length(3),
        Constraint::Length(1),
    ])
    .areas(area);
    draw_header(app, theme, header, buf);

    let wide = area.width >= WIDE_W;
    let mut cols = vec![
        Constraint::Length(METER_W),
        Constraint::Length(METER_W),
        Constraint::Fill(1),
    ];
    if wide {
        cols.push(Constraint::Length(HIST_W));
    }
    let cols = Layout::horizontal(cols).split(body);
    draw_meters(
        theme,
        "IN",
        &app.in_meters,
        app.in_clip.lit(app.now),
        cols[0],
        buf,
    );
    draw_meters(
        theme,
        "OUT",
        &app.out_meters,
        app.out_clip.lit(app.now),
        cols[1],
        buf,
    );
    if app.picker.is_some() {
        draw_picker(app, theme, cols[2], buf);
        if wide {
            draw_histogram(app, theme, cols[3], buf);
        }
    } else if app.latency.is_some() {
        draw_latency(app, theme, cols[2], buf);
        if wide {
            draw_histogram(app, theme, cols[3], buf);
        }
    } else if wide {
        draw_chain(app, theme, cols[2], buf);
        draw_histogram(app, theme, cols[3], buf);
    } else if app.show_histogram {
        draw_histogram(app, theme, cols[2], buf);
    } else {
        draw_chain(app, theme, cols[2], buf);
    }
    draw_engine(app, theme, engine, buf);
    draw_hints(app, theme, hints, buf);
    if app.help {
        draw_help(theme, area, buf);
    }
}

fn put(buf: &mut Buffer, x: u16, y: u16, s: &str, style: Style) -> u16 {
    let area = *buf.area();
    if y < area.top() || y >= area.bottom() || x >= area.right() {
        return x;
    }
    buf.set_stringn(x, y, s, usize::from(area.right() - x), style);
    x.saturating_add(u16::try_from(s.chars().count()).unwrap_or(u16::MAX))
}

fn boxed(theme: &Theme, title: &str, area: Rect, buf: &mut Buffer) -> Rect {
    let block = Block::default()
        .borders(Borders::ALL)
        .border_type(theme.glyphs.border)
        .border_style(theme.style(Role::Dim))
        .title(format!(" {title} "));
    let inner = block.inner(area);
    block.render(area, buf);
    inner
}

fn sep(theme: &Theme) -> &'static str {
    if theme.rich { " · " } else { " | " }
}

fn ell(theme: &Theme) -> &'static str {
    if theme.rich { "…" } else { "..." }
}

fn fit(s: &str, width: usize, ell: &str) -> String {
    if s.chars().count() <= width {
        return s.to_owned();
    }
    let keep = width.saturating_sub(ell.chars().count());
    s.chars().take(keep).chain(ell.chars()).collect()
}

fn wrap(s: &str, width: usize, lines: usize, ell: &str) -> Vec<String> {
    if width == 0 || lines == 0 {
        return Vec::new();
    }
    let chars: Vec<char> = s.chars().collect();
    let mut out: Vec<String> = chars.chunks(width).map(|c| c.iter().collect()).collect();
    if out.len() > lines {
        out.truncate(lines);
        if let Some(last) = out.last_mut() {
            let longer = format!("{last}{ell}");
            *last = fit(&longer, width, ell);
        }
    }
    out
}

fn fmt_ns(theme: &Theme, ns: u64) -> String {
    let ns = ns as f64;
    if ns >= 1e6 {
        format!("{:.2} ms", ns / 1e6)
    } else {
        format!("{:.0}{}s", ns / 1e3, if theme.rich { "µ" } else { "u" })
    }
}

fn draw_header(app: &App, theme: &Theme, area: Rect, buf: &mut Buffer) {
    let g = &theme.glyphs;
    let d = &app.device;
    let bar = if theme.rich { "│" } else { "|" };
    let sep = sep(theme);
    let ell = ell(theme);
    let mut left = format!(
        " rt-rig {bar} {}{sep}{}{sep}{} fr{sep}{:.2} ms",
        d.backend,
        khz(d.sample_rate),
        d.block_frames,
        d.block_ms()
    );

    let mut why = String::new();
    let mut note = String::new();
    let (icon, head, role) = match app.status() {
        Status::Live => (g.live, "LIVE".to_owned(), Role::Good),
        Status::Stalled(q) => (
            g.stalled,
            format!("STALLED{sep}no audio for {:.1} s", q.as_secs_f64()),
            Role::Warn,
        ),
        Status::Stopped => {
            why = if app.snapshot.device_error.is_empty() {
                "device stopped".to_owned()
            } else {
                app.snapshot.device_error.clone()
            };
            if app.mode() == Mode::Rig {
                note = format!("{sep}retrying{sep}o devices");
            }
            (g.stopped, format!("STOPPED{sep}"), Role::Bad)
        }
    };
    let flash = app.stats.xrun_flash(app.now);
    let marker = if flash {
        format!(" {}", g.xrun)
    } else {
        String::new()
    };
    let mut badge = String::new();
    if app.latency.as_ref().is_some_and(|l| l.status.latency_mode) {
        badge += &format!("{} SILENT  ", g.stopped);
    }
    if app.bypassed() {
        badge += &format!("{} BYPASS  ", g.stopped);
    }
    let mut tail = format!(" {bar} xruns {}{marker} ", app.stats.dropouts());

    let w = |s: &str| s.chars().count();
    let avail = usize::from(area.width);
    let fixed = w(&badge) + w(icon) + 1 + w(&head) + w(&note);
    let used = |l: &str, r: &str, t: &str| w(l) + 1 + fixed + w(r) + w(t);
    if used(&left, &why, &tail) > avail {
        tail = " ".to_owned();
    }
    if used(&left, &why, &tail) > avail {
        let over = used(&left, &why, &tail) - avail;
        let floor = w(&format!(" rt-rig {bar}"));
        left = fit(&left, w(&left).saturating_sub(over).max(floor + 1), ell);
    }
    if used(&left, &why, &tail) > avail && !why.is_empty() {
        let over = used(&left, &why, &tail) - avail;
        why = fit(&why, w(&why).saturating_sub(over), ell);
    }
    put(buf, area.x, area.y, &left, theme.style(Role::Title));

    let status = format!("{icon} {head}{why}{note}");
    let width = w(&badge) + w(&status) + w(&tail);
    let right = area.x + area.width;
    let mut x = right
        .saturating_sub(u16::try_from(width).unwrap_or(area.width))
        .max(area.x);
    x = put(buf, x, area.y, &badge, theme.style(Role::Bad));
    x = put(buf, x, area.y, &status, theme.style(role));
    put(
        buf,
        x,
        area.y,
        &tail,
        theme.style(if flash { Role::Bad } else { Role::Normal }),
    );
}

fn draw_meters(
    theme: &Theme,
    title: &str,
    meters: &[Meter],
    clip: bool,
    area: Rect,
    buf: &mut Buffer,
) {
    let inner = boxed(theme, title, area, buf);
    if inner.height < 5 || inner.width < 8 {
        return;
    }
    let normal = theme.style(Role::Normal);
    for (ch, label) in ["L", "R"].iter().enumerate().take(meters.len()) {
        put(buf, inner.x + 1 + 4 * ch as u16, inner.y, label, normal);
    }
    if clip {
        let style = theme.style(Role::Bad).add_modifier(Modifier::REVERSED);
        put(buf, inner.x + 2, inner.y + 1, "CLIP", style);
    }
    let rows = inner.height - 3;
    for (ch, m) in meters.iter().take(2).enumerate() {
        draw_vbar(
            theme,
            m,
            inner.x + 1 + 4 * ch as u16,
            inner.y + 2,
            rows,
            buf,
        );
        let text = readout(theme, m.level_db());
        put(
            buf,
            inner.x + 4 * ch as u16,
            inner.y + inner.height - 1,
            &text,
            normal,
        );
    }
}

fn readout(theme: &Theme, db: f64) -> String {
    if db <= FLOOR_DB {
        return "  -".to_owned();
    }
    let s = format!("{db:>3.0}");
    if theme.rich {
        s.replace('-', theme.glyphs.minus)
    } else {
        s
    }
}

fn draw_vbar(theme: &Theme, m: &Meter, x: u16, top: u16, rows: u16, buf: &mut Buffer) {
    let span = -FLOOR_DB;
    let cells = |db: f64| ((db - FLOOR_DB) / span).clamp(0.0, 1.0) * f64::from(rows);
    let level = cells(m.level_db());
    let hold_row =
        (m.hold_db() > FLOOR_DB).then(|| (cells(m.hold_db()).floor() as u16).min(rows - 1));
    for r in 0..rows {
        let y = top + rows - 1 - r;
        let row_top_db = FLOOR_DB + (f64::from(r) + 1.0) / f64::from(rows) * span;
        let mut sym = theme.glyphs.vbar_cell(level - f64::from(r));
        if sym == " " && hold_row == Some(r) {
            sym = theme.glyphs.hold;
        }
        put(buf, x, y, &sym.repeat(2), theme.style(zone(row_top_db)));
    }
}

fn draw_chain(app: &App, theme: &Theme, area: Rect, buf: &mut Buffer) {
    let inner = boxed(theme, "CHAIN", area, buf);
    if inner.height < 3 {
        return;
    }
    let bottom = inner.y + inner.height;
    let selected = app.rows.get(app.selected).copied();
    let mut y = inner.y + 1;
    for s in 0..app.strips.len() {
        let params: Vec<usize> = app
            .rows
            .iter()
            .filter(|r| r.strip == s)
            .map(|r| r.param)
            .collect();
        for (k, &p) in params.iter().enumerate() {
            if y + 1 >= bottom {
                break;
            }
            let row = Row { strip: s, param: p };
            chain_row(
                app,
                theme,
                buf,
                (inner.x, y),
                row,
                k == 0,
                selected == Some(row),
            );
            y += 1;
        }
        if !params.is_empty() {
            y += 1;
        }
    }
    let frames: i32 = app.strips.iter().map(|s| s.latency_frames).sum();
    let ms = if app.device.sample_rate > 0.0 {
        1000.0 * f64::from(frames) / app.device.sample_rate
    } else {
        0.0
    };
    let text = format!("chain latency {frames} fr{}{ms:.2} ms", sep(theme));
    put(buf, inner.x + 3, bottom - 1, &text, theme.style(Role::Dim));
}

fn chain_row(
    app: &App,
    theme: &Theme,
    buf: &mut Buffer,
    at: (u16, u16),
    row: Row,
    first: bool,
    selected: bool,
) {
    let (x0, y) = at;
    let strip = &app.strips[row.strip];
    let p = &strip.params[row.param];
    let v = app.values[row.strip][row.param];
    let dim = !app.strip_on(row.strip) || (row.strip != 0 && app.bypassed());
    let base = theme.style(if dim { Role::Dim } else { Role::Normal });
    let body = if selected {
        base.patch(theme.style(Role::Selected))
    } else {
        base
    };

    let cursor = if selected { theme.glyphs.cursor } else { " " };
    let mut x = put(buf, x0, y, cursor, theme.style(Role::Accent));
    let head = if first {
        format!("{} {}", row.strip + 1, name_cell(app, row.strip))
    } else {
        " ".repeat(13)
    };
    x = put(
        buf,
        x,
        y,
        &head,
        if first && !dim {
            theme.style(Role::Title)
        } else {
            base
        },
    );
    x = put(buf, x, y, &format!("{:<6}", p.name), body);
    x = put(buf, x, y, &slider(theme, taper::to_position(p, v)), body);
    x = put(
        buf,
        x,
        y,
        &format!(" {:>9}", taper::format_value(p, v, theme.rich)),
        body,
    );
    if first {
        indicator(app, theme, buf, (x, y), row.strip);
    }
}

fn name_cell(app: &App, s: usize) -> String {
    let strip = &app.strips[s];
    match strip.toggle_index() {
        Some(_) if s != 0 => {
            let name: String = strip.name.to_uppercase().chars().take(8).collect();
            format!("{name:<8}{:<3}", if app.strip_on(s) { "ON" } else { "OFF" })
        }
        _ => {
            let name: String = strip.name.to_uppercase().chars().take(11).collect();
            format!("{name:<11}")
        }
    }
}

fn slider(theme: &Theme, pos: f64) -> String {
    let g = &theme.glyphs;
    let knob = (pos.clamp(0.0, 1.0) * (SLIDER_W - 1) as f64).round() as usize;
    (0..SLIDER_W)
        .map(|i| {
            if i < knob {
                g.slider_fill
            } else if i == knob {
                g.slider_knob
            } else {
                g.slider_empty
            }
        })
        .collect()
}

fn indicator(app: &App, theme: &Theme, buf: &mut Buffer, at: (u16, u16), s: usize) {
    let (x, y) = at;
    let g = &theme.glyphs;
    let strip = &app.strips[s];
    let Some((i, p)) = strip.params.iter().enumerate().find(|(_, p)| p.read_only) else {
        return;
    };
    let v = app.values[s][i];
    if p.id == "gain_reduction" {
        let (led, word, role) = if v > -3.0 {
            (g.led_on, "OPEN", Role::Good)
        } else {
            (g.led_off, "GATED", Role::Dim)
        };
        put(buf, x, y, &format!("  {led} {word}"), theme.style(role));
    } else {
        let x = put(buf, x, y, &format!("  {} ", p.name), theme.style(Role::Dim));
        let filled = taper::to_position(p, v) * 6.0;
        let bar: String = (0..6).map(|c| g.hbar_cell(filled - f64::from(c))).collect();
        put(buf, x, y, &bar, theme.style(Role::Accent));
    }
}

fn load_role(r: f64) -> Role {
    if r > 0.8 {
        Role::Bad
    } else if r > 0.5 {
        Role::Warn
    } else {
        Role::Good
    }
}

fn draw_engine(app: &App, theme: &Theme, area: Rect, buf: &mut Buffer) {
    let inner = boxed(theme, "ENGINE", area, buf);
    let st = &app.stats;
    let y = inner.y;
    let (open, close) = if theme.rich {
        ("▕", "▏")
    } else {
        ("[", "]")
    };
    let normal = theme.style(Role::Normal);
    let role = load_role(st.last_load);

    let mut x = put(buf, inner.x, y, " load p99 ", normal);
    x = put(buf, x, y, open, theme.style(Role::Dim));
    let filled = st.last_load.clamp(0.0, 1.0) * 20.0;
    let bar: String = (0..20)
        .map(|c| theme.glyphs.hbar_cell(filled - f64::from(c)))
        .collect();
    x = put(buf, x, y, &bar, theme.style(role));
    x = put(buf, x, y, close, theme.style(Role::Dim));
    x = put(
        buf,
        x,
        y,
        &format!(" {:>4.0} %", st.last_load * 100.0),
        theme.style(role),
    );
    x = put(
        buf,
        x,
        y,
        &format!("   p99.9 {:.0} %", st.ratio(st.run_p999_ns) * 100.0),
        normal,
    );
    x = put(
        buf,
        x,
        y,
        &format!("   max {:.0} %   ", st.ratio(st.run_max_ns) * 100.0),
        normal,
    );

    let room = usize::from((inner.x + inner.width).saturating_sub(x + 1));
    let skip = st.load_history.len().saturating_sub(room);
    let spark: String = st
        .load_history
        .iter()
        .skip(skip)
        .map(|&v| theme.glyphs.vbar_cell(v))
        .collect();
    put(buf, x, y, &spark, theme.style(Role::Accent));
}

fn draw_histogram(app: &App, theme: &Theme, area: Rect, buf: &mut Buffer) {
    let title = if theme.rich {
        "CALLBACK TIME · run"
    } else {
        "CALLBACK TIME - run"
    };
    let inner = boxed(theme, title, area, buf);
    if inner.height < 6 || inner.width < 20 {
        return;
    }
    let g = &theme.glyphs;
    let st = &app.stats;
    let cols = usize::from(inner.width);
    let rows = inner.height - 4;
    let lo = 1e4_f64.log10();
    let hi = (st.deadline_ns as f64 * 1.5).max(1e6).log10();
    let col_of = |ns: f64| -> usize {
        let t = (ns.max(1.0).log10() - lo) / (hi - lo);
        (t.clamp(0.0, 1.0) * (cols - 1) as f64).round() as usize
    };

    let mut counts = vec![0u64; cols];
    for (b, &c) in st.run_histogram().iter().enumerate() {
        if c > 0 {
            counts[col_of(app.bucket_upper_ns[b] as f64)] += c;
        }
    }
    let peak = counts.iter().copied().max().unwrap_or(0);
    let deadline_col = (st.deadline_ns > 0).then(|| col_of(st.deadline_ns as f64));

    for (c, &n) in counts.iter().enumerate() {
        let h = if peak == 0 || n == 0 {
            0.0
        } else {
            (n as f64).ln_1p() / (peak as f64).ln_1p() * f64::from(rows)
        };
        for r in 0..rows {
            let mut sym = g.vbar_cell(h - f64::from(r));
            let mut style = theme.style(Role::Accent);
            if sym == " " && deadline_col == Some(c) {
                sym = g.deadline;
                style = theme.style(Role::Bad);
            }
            put(buf, inner.x + c as u16, inner.y + rows - 1 - r, sym, style);
        }
    }

    let (rule, tick) = if theme.rich {
        ("─", "┴")
    } else {
        ("-", "+")
    };
    let axis: String = (0..cols)
        .map(|c| if deadline_col == Some(c) { tick } else { rule })
        .collect();
    let dim = theme.style(Role::Dim);
    put(buf, inner.x, inner.y + rows, &axis, dim);

    let label_y = inner.y + rows + 1;
    let mut labels = vec![
        (0, fmt_ns(theme, 10_000)),
        (col_of(1e5), fmt_ns(theme, 100_000)),
        (col_of(1e6), fmt_ns(theme, 1_000_000)),
    ];
    if let Some(c) = deadline_col {
        labels.push((c, fmt_ns(theme, st.deadline_ns)));
    }
    for (c, text) in labels {
        let len = text.chars().count();
        let start = c.saturating_sub(len / 2).min(cols.saturating_sub(len));
        put(buf, inner.x + start as u16, label_y, &text, dim);
    }

    let normal = theme.style(Role::Normal);
    let line1 = format!(
        " p50 {}  p99 {}  p99.9 {}",
        fmt_ns(theme, st.run_p50_ns),
        fmt_ns(theme, st.run_p99_ns),
        fmt_ns(theme, st.run_p999_ns)
    );
    let line2 = format!(
        " max {}   {} deadline {}",
        fmt_ns(theme, st.run_max_ns),
        g.deadline,
        fmt_ns(theme, st.deadline_ns)
    );
    put(buf, inner.x, label_y + 1, &line1, normal);
    put(buf, inner.x, label_y + 2, &line2, normal);
}

fn draw_picker(app: &App, theme: &Theme, area: Rect, buf: &mut Buffer) {
    let Some(p) = app.picker.as_ref() else {
        return;
    };
    let inner = boxed(theme, "DEVICE", area, buf);
    if inner.height < 12 || inner.width < 40 {
        return;
    }
    let g = &theme.glyphs;
    let sep = sep(theme);
    let ell = ell(theme);
    let width = usize::from(inner.width);
    let normal = theme.style(Role::Normal);
    let dim = theme.style(Role::Dim);
    let x0 = inner.x;
    let mut y = inner.y + 1;

    let backend = format!("  {:<8}{}", "backend", p.edited.backend.to_uppercase());
    put(buf, x0, y, &backend, normal);
    y += 1;

    let selected = p.selected();
    for field in p.visible_fields() {
        let is_sel = field == selected;
        let read_only = p.read_only(field);
        let cursor = if is_sel { g.cursor } else { " " };
        let mut x = put(buf, x0, y, cursor, theme.style(Role::Accent));
        x = put(buf, x, y, &format!(" {:<8}", field.name()), normal);
        let value = p.label(field, app.device.sample_rate, sep);
        let text = if read_only {
            format!("  {value}")
        } else if matches!(field, Field::Input | Field::Output) {
            let w = width.saturating_sub(14);
            format!(
                "{} {:<w$} {}",
                g.step_prev,
                fit(&value, w, ell),
                g.step_next
            )
        } else {
            format!("{} {value} {}", g.step_prev, g.step_next)
        };
        let style = if is_sel {
            normal.patch(theme.style(Role::Selected))
        } else if read_only {
            dim
        } else {
            normal
        };
        x = put(buf, x, y, &text, style);
        let note = p.note(field);
        if !note.is_empty() {
            put(buf, x + 3, y, note, dim);
        }
        y += 1;
    }

    y += 1;
    let d = &app.device;
    let now = format!(
        "  now  {}{sep}{}{sep}{} fr",
        d.backend,
        khz(d.sample_rate),
        d.block_frames
    );
    put(buf, x0, y, &fit(&now, width, ell), normal);
    put(
        buf,
        x0,
        y + 1,
        &fit(&format!("       in  {}", d.input), width, ell),
        dim,
    );
    put(
        buf,
        x0,
        y + 2,
        &fit(&format!("       out {}", d.output), width, ell),
        dim,
    );
    y += 4;

    let (status, role) = if app.switching {
        (format!("switching{ell}"), Role::Warn)
    } else if let Some(e) = &p.list_error {
        (format!("device list unavailable: {e}"), Role::Bad)
    } else {
        match &p.status {
            PickerStatus::Applied => ("applied".to_owned(), Role::Good),
            PickerStatus::RolledBack(m) => (format!("rolled back: {m}"), Role::Warn),
            PickerStatus::Idle | PickerStatus::Pending => (String::new(), Role::Normal),
        }
    };
    let bottom = inner.y + inner.height;
    let lines = usize::from(bottom.saturating_sub(y)).min(3);
    for (i, line) in wrap(&status, width.saturating_sub(2), lines, ell)
        .iter()
        .enumerate()
    {
        put(buf, x0 + 2, y + i as u16, line, theme.style(role));
    }
}

fn draw_latency(app: &App, theme: &Theme, area: Rect, buf: &mut Buffer) {
    let Some(l) = app.latency.as_ref() else {
        return;
    };
    let inner = boxed(theme, "LATENCY", area, buf);
    if inner.height < 12 || inner.width < 40 {
        return;
    }
    let g = &theme.glyphs;
    let sep = sep(theme);
    let ell = ell(theme);
    let width = usize::from(inner.width);
    let normal = theme.style(Role::Normal);
    let dim = theme.style(Role::Dim);
    let x0 = inner.x;
    let bottom = inner.y + inner.height;
    let mut y = inner.y + 1;

    let arrow = if theme.rich { " → " } else { " -> " };
    let pair = format!(
        "  {:<9}{}{arrow}{}",
        "pair", app.device.input, app.device.output
    );
    put(buf, x0, y, &fit(&pair, width, ell), normal);
    y += 1;

    let control_failed =
        l.status.kind == LatencyKind::Control && l.status.state == LatencyState::Failed;
    let (control, role) = if l.status.control_passed {
        ("passed", Role::Good)
    } else if control_failed {
        ("FAILED", Role::Bad)
    } else {
        ("not run", Role::Warn)
    };
    let x = put(buf, x0, y, &format!("  {:<9}", "control"), normal);
    put(buf, x, y, control, theme.style(role));
    y += 1;

    let db = format!("{:.0}", l.level_db()).replace('-', g.minus);
    put(
        buf,
        x0,
        y,
        &format!("  {:<9}{db} dBFS{sep}5 repeats", "sweep"),
        normal,
    );
    y += 2;

    let pm = if theme.rich { "±" } else { "+" };
    let with_unaccounted = width >= 66;
    let mut head = format!(
        "  {:<17}{:>5}  {:<10}{:>7}{:>6}",
        "config", "ring", "measured", "buffer", "chain"
    );
    if with_unaccounted {
        head += &format!("{:>8}", "unacc.");
    }
    put(buf, x0, y, &head, dim);
    y += 1;

    let rows_end = bottom.saturating_sub(4);
    for r in app.latency_rows.iter().rev() {
        if y >= rows_end {
            break;
        }
        let chain = r
            .chain_ms
            .map_or_else(|| "n/a".to_owned(), |c| format!("{c:.2}"));
        let mut line = format!(
            "  {:<17}{:>5}{:>7.2}{pm}{:<4.2}{:>7.2}{:>6}",
            fit(&r.label, 17, ell),
            r.ring_blocks,
            r.measured_ms,
            r.spread_ms,
            r.computed_ms,
            chain
        );
        if with_unaccounted {
            line += &format!("{:>8.2}", r.unaccounted_ms());
        }
        if r.unstable {
            line += " UNSTABLE";
        } else if r.clipped {
            line += " clip";
        }
        put(buf, x0, y, &line, normal);
        y += 1;
    }

    let cursor = g.cursor;
    let accent = theme.style(Role::Accent);
    let warn = theme.style(Role::Warn);
    let lines: Vec<(String, Style)> = match l.step {
        Step::ConfirmControl => vec![
            (
                format!("{cursor} Move the headphones AWAY from the microphone."),
                accent,
            ),
            (format!("  Enter start{sep}Esc back"), dim),
        ],
        Step::ConfirmMeasure => vec![
            (
                format!("{cursor} Put the headphones AGAINST the microphone."),
                accent,
            ),
            (format!("  Enter start{sep}Esc back"), dim),
        ],
        Step::ConfirmLeave => vec![
            (
                format!("{cursor} Move the headphones AWAY from the microphone."),
                accent,
            ),
            (
                format!("  The rig's audio comes back.{sep}Enter leave{sep}Esc stay"),
                dim,
            ),
        ],
        Step::Running => {
            let what = match (l.status.kind, l.status.phase) {
                (LatencyKind::Control, _) => "control".to_owned(),
                (LatencyKind::Measure, LatencyPhase::Direct) => format!("measuring{sep}direct"),
                (LatencyKind::Measure, LatencyPhase::Chain) => format!("measuring{sep}chain"),
            };
            vec![
                (
                    format!(
                        "  {what}{sep}repeat {}/{}",
                        l.status.repeat.max(1),
                        l.status.repeats
                    ),
                    warn,
                ),
                ("  Esc cancel".to_owned(), dim),
            ]
        }
        Step::Home => {
            if let Some(m) = &l.message {
                wrap(m, width.saturating_sub(2), 3, ell)
                    .into_iter()
                    .map(|s| (format!("  {s}"), warn))
                    .collect()
            } else if app.latency_rows.last().is_some_and(|r| r.unstable) {
                let lags: Vec<String> = l
                    .status
                    .direct
                    .iter()
                    .map(|d| format!("{:.2}", d.lag_ms))
                    .collect();
                vec![(
                    fit(&format!("  repeats {}", lags.join(" ")), width, ell),
                    dim,
                )]
            } else {
                Vec::new()
            }
        }
    };
    let prompt_y = bottom.saturating_sub(3);
    for (i, (text, style)) in lines.iter().enumerate() {
        put(buf, x0, prompt_y + i as u16, text, *style);
    }
}

fn draw_hints(app: &App, theme: &Theme, area: Rect, buf: &mut Buffer) {
    if let Some(m) = app.message() {
        put(buf, area.x + 1, area.y, m, theme.style(Role::Warn));
        return;
    }
    let g = &theme.glyphs;
    let text = if app.mode() == Mode::Picker {
        format!(
            " {} field  {} change  R rescan  Esc close  qq quit",
            g.key_updown, g.key_leftright
        )
    } else if app.mode() == Mode::Latency {
        " c control  Enter measure  [ ] level  o devices  Esc leave".to_owned()
    } else {
        let rig = format!(
            " {} bypass  d drive  g gate  {} select  {} adjust  [ ] fine  ? help  qq quit",
            g.key_space, g.key_updown, g.key_leftright
        );
        let more = "  o devices";
        if rig.chars().count() + more.len() <= usize::from(area.width) {
            rig + more
        } else {
            rig
        }
    };
    put(buf, area.x, area.y, &text, theme.style(Role::Dim));
}

fn too_small(theme: &Theme, area: Rect, buf: &mut Buffer) {
    Clear.render(area, buf);
    let t = theme.glyphs.times;
    let msg = format!(
        "rt-rig needs {MIN_W}{t}{MIN_H} (now {}{t}{})",
        area.width, area.height
    );
    let len = u16::try_from(msg.chars().count()).unwrap_or(u16::MAX);
    let x = area.x + area.width.saturating_sub(len) / 2;
    put(
        buf,
        x,
        area.y + area.height / 2,
        &msg,
        theme.style(Role::Warn),
    );
}

fn draw_help(theme: &Theme, area: Rect, buf: &mut Buffer) {
    let w = 44u16.min(area.width);
    let h = (HELP.len() as u16 + 2).min(area.height);
    let r = Rect::new(
        area.x + (area.width - w) / 2,
        area.y + (area.height - h) / 2,
        w,
        h,
    );
    Clear.render(r, buf);
    let inner = boxed(theme, "KEYS", r, buf);
    for (i, (k, d)) in HELP.iter().enumerate() {
        put(
            buf,
            inner.x + 1,
            inner.y + i as u16,
            &format!("{k:<8}{d}"),
            theme.style(Role::Normal),
        );
    }
}

#[derive(Default)]
pub struct Fx {
    manager: EffectManager<()>,
    started: bool,
    last_bypass: Option<Instant>,
}

impl Fx {
    pub fn apply(
        &mut self,
        app: &App,
        theme: &Theme,
        elapsed: Duration,
        area: Rect,
        buf: &mut Buffer,
    ) {
        if !theme.glyphs.effects {
            return;
        }
        if !self.started {
            self.started = true;
            self.manager
                .add_effect(fx::sweep_in(Motion::LeftToRight, 10, 0, Color::Black, 800));
        }
        if app.bypass_changed != self.last_bypass {
            self.last_bypass = app.bypass_changed;
            self.manager.add_effect(fx::fade_from_fg(Color::Red, 300));
        }
        self.manager.process_effects(elapsed.into(), buf, area);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::app::Msg;
    use crate::model::{Engine, FakeEngine};
    use crate::theme::ColorMode;
    use ratatui::Terminal;
    use ratatui::backend::TestBackend;

    fn app_with(e: &mut FakeEngine) -> App {
        let t0 = Instant::now();
        let mut app = App::new(&*e, t0);
        let snap = e.snapshot().unwrap();
        app.ingest(snap, &*e, t0);
        app
    }

    fn rows(app: &App, theme: &Theme, w: u16, h: u16) -> Vec<String> {
        let mut term = Terminal::new(TestBackend::new(w, h)).unwrap();
        term.draw(|f| render(app, theme, f)).unwrap();
        let buf = term.backend().buffer();
        (0..h)
            .map(|y| (0..w).map(|x| buf[(x, y)].symbol()).collect())
            .collect()
    }

    fn has(rows: &[String], s: &str) -> bool {
        rows.iter().any(|r| r.contains(s))
    }

    #[test]
    fn rich_80x24_matches_the_mockup_landmarks() {
        let mut e = FakeEngine::rig();
        let app = app_with(&mut e);
        let r = rows(&app, &Theme::new(true, ColorMode::TrueColor), 80, 24);
        assert!(
            r[0].contains("rt-rig") && r[0].contains("LIVE") && r[0].contains("xruns 0"),
            "{}",
            r[0]
        );
        assert!(r[1].starts_with('╭'));
        assert!(has(&r, "▸1 MASTER"));
        assert!(has(&r, "GATE    ON thr"));
        assert!(has(&r, "● OPEN"));
        assert!(has(&r, "DRIVE   ON drive"));
        assert!(has(&r, "chain latency 16 fr"));
        assert!(has(&r, "ENGINE"));
        assert!(r[23].contains("qq quit"));
        assert!(!has(&r, "CALLBACK TIME"));
    }

    #[test]
    fn basic_mode_stays_in_the_console_font() {
        const SAFE: &str = "─│┌┐└┘├┤┬┴┼█▄▀▌▐░▒▓";
        let mut e = FakeEngine::rig();
        let app = app_with(&mut e);
        let r = rows(&app, &Theme::new(false, ColorMode::None), 80, 24);
        for row in &r {
            for c in row.chars() {
                assert!(c.is_ascii() || SAFE.contains(c), "{c:?} in {row:?}");
            }
        }
        assert!(r[1].starts_with('┌'));
        assert!(r[23].contains("qq quit"));
    }

    #[test]
    fn switches_and_bypass_show_as_text() {
        let mut e = FakeEngine::rig();
        let mut app = app_with(&mut e);
        let now = app.now;
        app.update(Msg::ToggleDrive, &mut e, now).unwrap();
        app.update(Msg::ToggleBypass, &mut e, now).unwrap();
        let r = rows(&app, &Theme::new(true, ColorMode::None), 80, 24);
        assert!(has(&r, "DRIVE   OFF"));
        assert!(r[0].contains("BYPASS"));
    }

    #[test]
    fn gate_led_shows_gated() {
        let mut e = FakeEngine::rig();
        e.values[2][2] = -20.0;
        let app = app_with(&mut e);
        assert!(has(
            &rows(&app, &Theme::new(true, ColorMode::None), 80, 24),
            "○ GATED"
        ));
    }

    #[test]
    fn too_small_terminal_shows_only_a_message() {
        let mut e = FakeEngine::rig();
        let app = app_with(&mut e);
        let r = rows(&app, &Theme::new(true, ColorMode::None), 72, 20);
        assert!(has(&r, "rt-rig needs 80×24 (now 72×20)"));
        assert!(!has(&r, "CHAIN"));
    }

    #[test]
    fn histogram_panel_appears_when_wide_or_swapped() {
        let mut e = FakeEngine::rig();
        e.next.hist_window[20] = 50;
        let mut app = app_with(&mut e);
        let t = Theme::new(true, ColorMode::None);
        let wide = rows(&app, &t, 120, 30);
        assert!(has(&wide, "CALLBACK TIME") && has(&wide, "CHAIN"));
        assert!(has(&wide, "deadline 2.67 ms"));
        app.show_histogram = true;
        let narrow = rows(&app, &t, 80, 24);
        assert!(has(&narrow, "CALLBACK TIME") && !has(&narrow, "CHAIN"));
    }

    #[test]
    fn stopped_device_shows_its_error() {
        let mut e = FakeEngine::rig();
        e.next.running = false;
        e.next.device_error = "ALSA transfer failed: x".into();
        let app = app_with(&mut e);
        let r = rows(&app, &Theme::new(true, ColorMode::None), 80, 24);
        assert!(
            r[0].contains("STOPPED") && r[0].contains("ALSA transfer failed"),
            "{}",
            r[0]
        );
    }

    #[test]
    fn help_overlay_and_quit_prompt() {
        let mut e = FakeEngine::rig();
        let mut app = app_with(&mut e);
        let now = app.now;
        let t = Theme::new(true, ColorMode::None);
        app.help = true;
        assert!(has(&rows(&app, &t, 80, 24), "reset stats"));
        app.help = false;
        app.update(Msg::Quit, &mut e, now).unwrap();
        assert!(rows(&app, &t, 80, 24)[23].contains("press q again to quit"));
    }

    #[test]
    fn effects_do_nothing_in_basic_mode() {
        let mut e = FakeEngine::rig();
        let app = app_with(&mut e);
        let t = Theme::new(false, ColorMode::None);
        let area = Rect::new(0, 0, 80, 24);
        let mut buf = Buffer::empty(area);
        draw(&app, &t, area, &mut buf);
        let before = buf.clone();
        Fx::default().apply(&app, &t, Duration::from_millis(100), area, &mut buf);
        assert_eq!(buf, before);
    }

    #[test]
    fn effects_run_in_rich_mode() {
        let mut e = FakeEngine::rig();
        let mut app = app_with(&mut e);
        let t = Theme::new(true, ColorMode::TrueColor);
        let area = Rect::new(0, 0, 80, 24);
        let mut buf = Buffer::empty(area);
        let mut fx = Fx::default();
        draw(&app, &t, area, &mut buf);
        fx.apply(&app, &t, Duration::from_millis(100), area, &mut buf);
        app.bypass_changed = Some(app.now);
        fx.apply(&app, &t, Duration::from_millis(1000), area, &mut buf);
    }

    fn picker_app(e: &mut FakeEngine, backend: &str) -> App {
        e.config.backend = backend.into();
        let mut app = app_with(e);
        let now = app.now;
        app.update(Msg::OpenPicker, e, now).unwrap();
        app
    }

    #[test]
    fn picker_replaces_the_chain_at_80x24() {
        let mut e = FakeEngine::rig();
        let app = picker_app(&mut e, "wasapi");
        let r = rows(&app, &Theme::new(true, ColorMode::TrueColor), 80, 24);
        assert!(has(&r, "DEVICE") && !has(&r, "CHAIN"));
        assert!(has(&r, "backend WASAPI"));
        assert!(has(&r, "▸ input   ◂ System default (Mic)"));
        assert!(has(&r, "mode    ◂ shared ▸"));
        assert!(has(&r, "◂ 128 fr · 2.67 ms ▸   the driver can round it"));
        assert!(has(&r, "48 kHz   set by the device mix format"));
        assert!(has(
            &r,
            "ring    ◂ 2 blocks ▸   lower: less delay, more risk"
        ));
        assert!(has(&r, "now  Null · 48 kHz · 128 fr"));
        assert!(r[23].contains("R rescan"), "{}", r[23]);
    }

    #[test]
    fn picker_basic_glyphs_stay_in_the_console_font() {
        const SAFE: &str = "─│┌┐└┘├┤┬┴┼█▄▀▌▐░▒▓";
        let mut e = FakeEngine::rig();
        let app = picker_app(&mut e, "wasapi");
        let r = rows(&app, &Theme::new(false, ColorMode::None), 80, 24);
        for row in &r {
            for c in row.chars() {
                assert!(c.is_ascii() || SAFE.contains(c), "{c:?} in {row:?}");
            }
        }
        assert!(has(&r, "> input   < System default (Mic)"));
        assert!(has(&r, "< 128 fr | 2.67 ms >"));
    }

    #[test]
    fn picker_status_lines() {
        let mut e = FakeEngine::rig();
        let mut app = picker_app(&mut e, "null");
        let t = Theme::new(true, ColorMode::None);
        app.switching = true;
        assert!(has(&rows(&app, &t, 80, 24), "switching…"));
        app.switching = false;
        app.picker.as_mut().unwrap().status = PickerStatus::RolledBack(
            "could not open the new config: cannot open usb; restored the previous config".into(),
        );
        let r = rows(&app, &t, 80, 24);
        assert!(has(&r, "rolled back: could not open the new config"));
        assert!(
            has(&r, "restored the previous config"),
            "wrapped onto a second line"
        );
        app.picker.as_mut().unwrap().list_error = Some("COM error".into());
        assert!(has(
            &rows(&app, &t, 80, 24),
            "device list unavailable: COM error"
        ));
    }

    #[test]
    fn picker_at_120_columns_keeps_the_histogram() {
        let mut e = FakeEngine::rig();
        let app = picker_app(&mut e, "null");
        let r = rows(&app, &Theme::new(true, ColorMode::None), 120, 30);
        assert!(has(&r, "DEVICE") && has(&r, "CALLBACK TIME") && !has(&r, "CHAIN"));
    }

    #[test]
    fn stopped_header_keeps_the_retry_hint_at_80_columns() {
        let mut e = FakeEngine::rig();
        e.device.backend = "WASAPI (exclusive) in f32 out f32 asrc 44100->48000".into();
        e.next.running = false;
        e.next.device_error = "WASAPI device lost (hr=0x88890004)".into();
        let app = app_with(&mut e);
        let r = rows(&app, &Theme::new(true, ColorMode::None), 80, 24);
        assert!(r[0].starts_with(" rt-rig │"), "{}", r[0]);
        assert!(r[0].contains("■ STOPPED · WASAPI device lost"), "{}", r[0]);
        assert!(r[0].contains("retrying · o devices"), "{}", r[0]);
    }

    #[test]
    fn stopped_header_in_the_picker_has_no_retry_hint() {
        let mut e = FakeEngine::rig();
        e.next.running = false;
        let mut app = picker_app(&mut e, "null");
        let snap = e.snapshot().unwrap();
        let now = app.now;
        app.ingest(snap, &e, now);
        let r = rows(&app, &Theme::new(true, ColorMode::None), 80, 24);
        assert!(
            r[0].contains("STOPPED") && !r[0].contains("retrying"),
            "{}",
            r[0]
        );
    }

    #[test]
    fn fit_and_wrap() {
        assert_eq!(fit("abcdef", 10, "…"), "abcdef");
        assert_eq!(fit("abcdef", 4, "…"), "abc…");
        assert_eq!(fit("abcdef", 4, "..."), "a...");
        assert_eq!(wrap("abcdefgh", 3, 2, "…"), ["abc", "de…"]);
        assert_eq!(wrap("abcd", 3, 2, "…"), ["abc", "d"]);
        assert!(wrap("", 3, 2, "…").is_empty());
    }

    #[test]
    fn the_rig_hint_shows_o_when_there_is_room() {
        let mut e = FakeEngine::rig();
        let app = app_with(&mut e);
        let t = Theme::new(true, ColorMode::None);
        assert!(!rows(&app, &t, 80, 24)[23].contains("o devices"));
        assert!(rows(&app, &t, 120, 30)[29].contains("o devices"));
    }

    use crate::latency::{LatencyRow, Step};
    use crate::model::{LatencyKind, LatencyPhase};

    fn latency_app(e: &mut FakeEngine) -> App {
        let mut app = app_with(e);
        let now = app.now;
        app.update(Msg::OpenLatency, e, now).unwrap();
        app
    }

    fn latency_row(label: &str, chain: Option<f64>, unstable: bool) -> LatencyRow {
        LatencyRow {
            label: label.into(),
            ring_blocks: 2.0,
            measured_ms: 21.89,
            spread_ms: 0.03,
            computed_ms: 6.33,
            chain_ms: chain,
            unstable,
            clipped: false,
        }
    }

    #[test]
    fn latency_panel_at_80x24() {
        let mut e = FakeEngine::rig();
        let mut app = latency_app(&mut e);
        app.latency.as_mut().unwrap().status.control_passed = true;
        app.latency_rows
            .push(latency_row("excl 144/48k", Some(0.41), false));
        let mut tight = latency_row("shared 1056/48k", None, true);
        tight.ring_blocks = 1.25;
        app.latency_rows.push(tight);
        let r = rows(&app, &Theme::new(true, ColorMode::TrueColor), 80, 24);
        assert!(has(&r, "LATENCY") && !has(&r, "CHAIN"));
        assert!(r[0].contains("■ SILENT"), "{}", r[0]);
        assert!(has(&r, "pair     synthetic tone → discard"));
        assert!(has(&r, "control  passed"));
        assert!(has(&r, "sweep    −6 dBFS · 5 repeats"));
        assert!(has(&r, "ring  measured   buffer chain"));
        assert!(has(&r, "excl 144/48k") && has(&r, "2  21.89±0.03   6.33  0.41"));
        assert!(has(&r, "shared 1056/48k   1.25  21.89±0.03"));
        assert!(has(&r, "n/a UNSTABLE"));
        assert!(!has(&r, "unacc."));
        assert!(r[23].contains("c control"), "{}", r[23]);
    }

    #[test]
    fn latency_panel_basic_glyphs_stay_in_the_console_font() {
        const SAFE: &str = "─│┌┐└┘├┤┬┴┼█▄▀▌▐░▒▓";
        let mut e = FakeEngine::rig();
        let mut app = latency_app(&mut e);
        app.latency_rows
            .push(latency_row("excl 144/48k", Some(0.41), false));
        let r = rows(&app, &Theme::new(false, ColorMode::None), 80, 24);
        for line in &r {
            for c in line.chars() {
                assert!(c.is_ascii() || SAFE.contains(c), "{c:?} in {line:?}");
            }
        }
        assert!(r[0].contains("# SILENT"), "{}", r[0]);
        assert!(has(&r, "synthetic tone -> discard"));
        assert!(has(&r, "21.89+0.03"));
        assert!(has(&r, "control  not run"));
    }

    #[test]
    fn latency_prompts_and_progress() {
        let mut e = FakeEngine::rig();
        let mut app = latency_app(&mut e);
        let t = Theme::new(true, ColorMode::None);

        app.latency.as_mut().unwrap().step = Step::ConfirmMeasure;
        let r = rows(&app, &t, 80, 24);
        assert!(has(&r, "▸ Put the headphones AGAINST the microphone."));
        assert!(has(&r, "Enter start · Esc back"));

        let s = app.latency.as_mut().unwrap();
        s.step = Step::Running;
        s.status.kind = LatencyKind::Measure;
        s.status.phase = LatencyPhase::Chain;
        s.status.repeat = 2;
        s.status.repeats = 5;
        assert!(has(
            &rows(&app, &t, 80, 24),
            "measuring · chain · repeat 2/5"
        ));

        let s = app.latency.as_mut().unwrap();
        s.step = Step::Home;
        s.message = Some("run the negative control first (c)".into());
        assert!(has(
            &rows(&app, &t, 80, 24),
            "run the negative control first (c)"
        ));
    }

    #[test]
    fn the_unaccounted_column_needs_room() {
        let mut e = FakeEngine::rig();
        let mut app = latency_app(&mut e);
        app.latency_rows
            .push(latency_row("excl 144/48k", Some(0.41), false));
        let t = Theme::new(true, ColorMode::None);
        let r100 = rows(&app, &t, 100, 30);
        assert!(has(&r100, "unacc.") && has(&r100, "15.56"));
        let r120 = rows(&app, &t, 120, 30);
        assert!(!has(&r120, "unacc."));
        assert!(has(&r120, "CALLBACK TIME") && has(&r120, "LATENCY"));
    }
}
