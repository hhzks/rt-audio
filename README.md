# rt-audio

Low-latency realtime audio passthrough with an effects chain. Windows (WASAPI) primary,
with a portable core designed so that adding ALSA/JACK is a contained change.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Windows:
```
cmake --preset windows-msvc
cmake --build --preset windows-msvc
```

## Run

```bash
build/src/app/rt_audio --list
build/src/app/rt_audio --backend null --block 128 --drive 5 --mix 0.7
build/tools/offline_render/offline_render --out sweep.wav --seconds 5 --drive 6 --mix 0.9
cmake --build build --target check_layering
```
