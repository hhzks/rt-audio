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
- Press `m` to measure the round-trip latency. The output is silent while the latency screen is
  open. Run the negative control first (`c`, with the headphones away from the microphone), then
  measure (`Enter`, with the headphones against the microphone). Each measurement adds a row with
  its config, so you can compare block sizes and modes after a change with `o`. When you quit,
  `rt_rig` prints the table.
- The `ring` field in the `o` panel, or `--ring`, sets the capture ring margin, from 1 to 2
  blocks (default 2). A lower value cuts the round trip by up to one block, but it gives less
  protection against underruns. Measure each value with `m`, and look at the underrun count in
  the header.

## ASIO® (Windows)

<img src="assets/asio-compatible.svg" width="120" alt="ASIO Compatible logo">

rt-audio can use ASIO® drivers on Windows. An ASIO driver gives input and output in one callback
on one clock, so the capture ring, the resampler and the drift loop are not used, and buffers
below the WASAPI minimum are possible.

The ASIO backend is not in the normal build. To build it:

```
cmake --preset windows-clang-cl-asio
cmake --build --preset windows-clang-cl-asio
```

CMake downloads the Steinberg ASIO SDK 2.3.4 and checks its SHA-256. The SDK is used under GPLv3,
so the binaries built with the ASIO backend are covered by GPLv3. The source in this repository
stays MIT. Each release has a separate `-asio` Windows package with the licence texts and the SDK
files that it uses.

Run with `--backend asio`. In `rt_rig`, the picker shows one `driver` field, and `p` opens the
driver's settings panel. Block 0 means the driver's preferred buffer size. A buffer size change in
the driver panel takes effect only when the block is 0; an explicit block size is requested again
when the device reopens.

With ASIO4ALL on onboard audio:
- In the ASIO4ALL panel, activate only the output and the input that you use. With all devices
  active, the stream may not start.
- Close the Windows Sound control panel and the Realtek Audio Console. They can hold the input, and
  ASIO4ALL then removes it without an error.

ASIO is a registered trademark of Steinberg Media Technologies GmbH. The ASIO Compatible logo is not
covered by the MIT licence. It is used under the Steinberg ASIO usage guidelines.

## License

MIT. See [LICENSE](LICENSE).
