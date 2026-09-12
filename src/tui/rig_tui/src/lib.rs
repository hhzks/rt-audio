pub mod engine;
pub mod ffi;

use std::io;
use std::panic::{self, AssertUnwindSafe};
use std::time::{Duration, Instant};

use clap::{Parser, ValueEnum};
use ratatui::DefaultTerminal;
use ratatui::crossterm::event;
use rig_ui::app::{App, map_key};
use rig_ui::model::{Engine, EngineError};
use rig_ui::theme::{ColorChoice, Env, GlyphChoice, Theme, pick_colors, pick_rich};
use rig_ui::view::{self, Fx};

use crate::engine::{FfiEngine, OpenOptions};

#[derive(Clone, Copy, Debug, ValueEnum)]
enum GlyphArg {
    Auto,
    Rich,
    Basic,
}

#[derive(Clone, Copy, Debug, ValueEnum)]
enum ColorArg {
    Auto,
    Truecolor,
    #[value(name = "16")]
    Sixteen,
    None,
}

#[derive(Parser, Debug)]
#[command(
    name = "rt_rig",
    version,
    about = "rt-rig: play through the rt-audio engine"
)]
struct Args {
    /// wasapi | alsa | null (default: platform default)
    #[arg(long)]
    backend: Option<String>,
    /// capture device id (default: system default)
    #[arg(long = "in")]
    input: Option<String>,
    /// render device id (default: system default)
    #[arg(long = "out")]
    output: Option<String>,
    /// requested sample rate in Hz
    #[arg(long, default_value_t = 48000.0)]
    rate: f64,
    /// requested block size in frames (0 = driver minimum)
    #[arg(long, default_value_t = 0)]
    block: i32,
    /// WASAPI exclusive mode
    #[arg(long)]
    exclusive: bool,
    /// UI frame rate
    #[arg(long, default_value_t = 30, value_parser = clap::value_parser!(u32).range(10..=60))]
    fps: u32,
    /// glyph set
    #[arg(long, value_enum, default_value_t = GlyphArg::Auto)]
    glyphs: GlyphArg,
    /// colour mode
    #[arg(long, value_enum, default_value_t = ColorArg::Auto)]
    color: ColorArg,
}

enum Fatal {
    Engine(EngineError),
    Terminal(io::Error),
}

/// # Safety
/// `out` must point to `cap` writable `u64` values, or be null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rt_tui_layout_probe(out: *mut u64, cap: usize) -> usize {
    let v = ffi::layout_values();
    if !out.is_null() {
        let n = v.len().min(cap);
        // SAFETY: the caller guarantees `cap` writable slots; we write at most `cap`.
        unsafe { std::ptr::copy_nonoverlapping(v.as_ptr(), out, n) };
    }
    v.len()
}

#[unsafe(no_mangle)]
pub extern "C" fn rt_tui_main() -> i32 {
    let args = match Args::try_parse_from(std::env::args_os()) {
        Ok(a) => a,
        Err(e) => {
            let _ = e.print();
            return e.exit_code();
        }
    };
    match panic::catch_unwind(AssertUnwindSafe(|| run(&args))) {
        Ok(code) => code,
        Err(_) => {
            eprintln!("rt-rig: exiting after a panic");
            101
        }
    }
}

fn run(args: &Args) -> i32 {
    let options = OpenOptions {
        backend: args.backend.clone(),
        input: args.input.clone(),
        output: args.output.clone(),
        rate: args.rate,
        block: args.block,
        exclusive: args.exclusive,
    };
    // Open the device before the alternate screen, so a failure stays visible.
    let mut engine = match FfiEngine::open(&options) {
        Ok(e) => e,
        Err(e) => {
            eprintln!("rt-rig: {e}");
            return match e {
                EngineError::Arg(_) => 2,
                EngineError::Internal(_) => 5,
                EngineError::Device(_) | EngineError::State(_) => 3,
            };
        }
    };

    let env = Env::from_process();
    let glyphs = match args.glyphs {
        GlyphArg::Auto => GlyphChoice::Auto,
        GlyphArg::Rich => GlyphChoice::Rich,
        GlyphArg::Basic => GlyphChoice::Basic,
    };
    let color = match args.color {
        ColorArg::Auto => ColorChoice::Auto,
        ColorArg::Truecolor => ColorChoice::TrueColor,
        ColorArg::Sixteen => ColorChoice::Sixteen,
        ColorArg::None => ColorChoice::None,
    };
    let theme = Theme::new(pick_rich(glyphs, &env), pick_colors(color, &env));

    let mut terminal = match ratatui::try_init() {
        Ok(t) => t,
        Err(e) => {
            eprintln!("rt-rig: terminal unusable: {e}");
            return 4;
        }
    };
    let result = ui_loop(&mut terminal, &mut engine, &theme, args.fps);
    ratatui::restore();
    drop(engine);

    match result {
        Ok(lines) => {
            for line in lines {
                println!("{line}");
            }
            0
        }
        Err(Fatal::Terminal(e)) => {
            eprintln!("rt-rig: terminal lost: {e}");
            4
        }
        Err(Fatal::Engine(e)) => {
            eprintln!("rt-rig: {e}");
            5
        }
    }
}

fn ui_loop(
    terminal: &mut DefaultTerminal,
    engine: &mut FfiEngine,
    theme: &Theme,
    fps: u32,
) -> Result<Vec<String>, Fatal> {
    let frame = Duration::from_secs(1) / fps;
    let mut app = App::new(&*engine, Instant::now());
    let mut fx = Fx::default();
    let mut last = Instant::now();
    loop {
        let next_frame = last + frame;
        while event::poll(next_frame.saturating_duration_since(Instant::now()))
            .map_err(Fatal::Terminal)?
        {
            let ev = event::read().map_err(Fatal::Terminal)?;
            if let Some(msg) = map_key(&ev, app.mode()) {
                app.update(msg, engine, Instant::now())
                    .map_err(Fatal::Engine)?;
            }
            if app.quit {
                return Ok([app.quit_line(&*engine), app.quit_table()]
                    .into_iter()
                    .flatten()
                    .collect());
            }
        }
        if let Some(cfg) = app.due(&*engine, Instant::now()) {
            app.switching = true;
            terminal
                .draw(|f| view::render(&app, theme, f))
                .map_err(Fatal::Terminal)?;
            let result = engine.reconfigure(&cfg);
            app.apply(result, &*engine, Instant::now())
                .map_err(Fatal::Engine)?;
        }
        let now = Instant::now();
        let snap = engine.snapshot().map_err(Fatal::Engine)?;
        app.ingest(snap, &*engine, now);
        app.poll_latency(&*engine).map_err(Fatal::Engine)?;
        let elapsed = now.saturating_duration_since(last);
        last = now;
        terminal
            .draw(|f| {
                view::render(&app, theme, f);
                let area = f.area();
                fx.apply(&app, theme, elapsed, area, f.buffer_mut());
            })
            .map_err(Fatal::Terminal)?;
    }
}
