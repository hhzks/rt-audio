# rt-audio

Low-latency realtime audio passthrough with an effects chain in C++26 with a Rust TUI frontend. Windows primary,
with an ALSA/JACK port in progress.

<img width="800" height="450" alt="rt-audio" src="https://github.com/user-attachments/assets/4cd0c155-bca4-4342-8fca-0cc456c0135c" />

## Requirements

- CMake >= 3.25
- **Windows:** LLVM/clang-cl >= 17 (`winget install LLVM.LLVM`), plus Visual Studio or Build Tools
  for the MSVC STL and Windows SDK. MSVC's `cl.exe` *cannot* build this project: there is no
  `/std:c++26` flag in any MSVC release, and updating Visual Studio does not change that. clang-cl
  keeps the MSVC ABI, so linking is unaffected.
- **Linux:** GCC >= 14 or Clang >= 17.
- **Terminal UI (optional):** Rust 1.98.1 through rustup
  (`rustup toolchain install 1.98.1 --profile minimal --component clippy,rustfmt`). Without `cargo`
  on `PATH`, CMake skips `rt_rig` and builds everything else.

## Build

Windows:
```
cmake --preset windows-clang-cl
cmake --build --preset windows-clang-cl
ctest --test-dir build/windows-clang --output-on-failure
```

Linux:
```bash
cmake --preset linux
cmake --build --preset linux
ctest --test-dir build/linux --output-on-failure
```

A `windows-msvc` preset exists only so the C++26 diagnostic is discoverable; it fails to configure
by design.

## Run

Paths below use the Windows build directory; on Linux substitute `build/linux`.

```bash
build/windows-clang/src/app/rt_audio --list
build/windows-clang/src/app/rt_audio --backend null --block 128 --drive 5 --mix 0.7
build/windows-clang/src/tui/rt_rig --backend null
build/windows-clang/tools/offline_render/offline_render --out sweep.wav --seconds 5 --drive 6 --mix 0.9
cmake --build build/windows-clang --target check_layering
```

## Terminal UI

`rt_rig` is a terminal UI for playing through the engine: level meters, the effect chain, on/off
switches, bypass, and callback timing against the block deadline. Press `?` in the app for the
keys; `q q` quits.

- It works in Windows Terminal, VS Code, Linux desktop terminals and the Linux text console. With
  `TERM=linux` it uses a reduced glyph set; `--glyphs` and `--color` override the detection.
- Over SSH, run it inside `tmux`, so a dropped connection does not stop the audio.
- `--fps 15` reduces CPU use on small boards.
- Press `o` to choose the input and output devices, the block size, the sample rate (ALSA and
  Null) and WASAPI shared or exclusive mode. A change applies after a 250 ms pause. If the new
  config fails to open, the previous one comes back. When you quit after a change, `rt_rig`
  prints the command line that starts it with the same config. The device ids in it are in
  single quotes, which PowerShell, POSIX shells and systemd accept; cmd.exe needs double quotes.
- If the device stops, for example because a USB cable comes out, `rt_rig` tries to open it
  again every 2 s until it comes back.

## License

MIT. See [LICENSE](LICENSE).
