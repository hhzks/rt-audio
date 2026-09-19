//! The rt-rig terminal UI. Pure Rust: no FFI, no unsafe.

pub mod app;
pub mod latency;
pub mod meters;
pub mod model;
pub mod picker;
pub mod stats;
pub mod taper;
pub mod theme;
pub mod view;

pub const ASIO_NOTICE: &str =
    "ASIO is a registered trademark of Steinberg Media Technologies GmbH.";
