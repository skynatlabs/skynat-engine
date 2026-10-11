# NOTICE

**skynat-engine** is a headless live-production engine built from **OBS Studio's libobs**.
It is free software: GNU General Public License, version 2 or (at your option) any later
version — the same licence as libobs. The full text is in [`COPYING`](COPYING) (copied from the
OBS Studio source tree).

"OBS" and "OBS Studio" are names of the OBS Project. This program is not made or endorsed by the
OBS Project; it is built *with* OBS Studio's engine. Skynat is the author of the code in `src/`,
`client/`, `tests/` and `scripts/`. `client/engine-client.ts` contains no OBS code (it only speaks
the pipe protocol) and is licensed MIT, as stated in that file, so apps of any licence can use it;
all other files are GPL-2.0-or-later.

## What is used from OBS Studio

| | |
|---|---|
| Repository | https://github.com/obsproject/obs-studio |
| Tag | **31.0.4** (cloned with `--depth 1 --recurse-submodules --shallow-submodules`) |
| Modifications to OBS | **None.** OBS is built from the unmodified tag. `patches/` is empty. |
| Prebuilt dependencies | https://github.com/obsproject/obs-deps release **2024-09-12** (`macos-deps-2024-09-12-universal.tar.xz`; OBS's own CMake also fetches `macos-deps-qt6-…`, which the engine does not use), downloaded by OBS's own CMake from `buildspec.json`, SHA-256 checked by it |
| Parts built | `libobs`, `libobs-opengl` (macOS) / `libobs-d3d11` (Windows) and the plugins `image-source`, `obs-transitions`, `obs-filters`, `obs-ffmpeg`, `obs-outputs`, `obs-x264`, plus `mac-avcapture`, `mac-videotoolbox`, `coreaudio-encoder` (macOS) or `win-dshow`, `obs-nvenc`, `obs-qsv11` (Windows) |
| Parts switched off | the Qt interface (`ENABLE_UI=OFF`), browser/CEF, scripting, obs-websocket, VST, AJA, Decklink, Syphon, virtual camera, VLC, FreeType text, WebRTC, SRT/RIST mpegts output, service updates; Sparkle (the updater) is part of the interface and is not built |

### Why 31.0.4

The Skynat Live app has three Mac builds: Apple Silicon, Intel, and "older Macs" (Intel, macOS 11.0,
Electron 37). One engine version should serve all three, so the tag's minimum macOS had to be
11.0. OBS sets that minimum in `CMakePresets.json` (`CMAKE_OSX_DEPLOYMENT_TARGET` of the `macos`
preset); checked on 11 Oct 2026:

| OBS tag | macOS minimum | Windows build preset |
|---|---|---|
| 30.2.3 | 11.0 | Visual Studio 17 2022, Windows SDK 10.0.20348+ |
| **31.0.4** | **11.0** | Visual Studio 17 2022, Windows SDK 10.0.22621 (≥ 10.0.20348 enforced) |
| 31.1.2 | 12.0 | Visual Studio 17 2022 |
| 32.0.4, 32.1.2 | 12.0 | Visual Studio 17 2022 |
| 32.2.2 | 13.0 (and Xcode/SDK 26.5) | Visual Studio 18 2026, SDK 10.0.26100 |
| 33.0.0-rc1 (not a release) | 13.0 | Visual Studio 18 2026 |

31.0.4 is the newest release whose macOS minimum is 11.0. It has everything the engine needs:
hardware H.264 through VideoToolbox (CBR on Apple Silicon, average bitrate with a ceiling on Intel),
CoreAudio AAC, the zero-copy "fast" AVFoundation camera source, the FFmpeg media source with
hardware decoding, extra video mixes (`obs_view_add2`), GPU-scaled encoders
(`obs_encoder_set_gpu_scale_type`) with frame-rate divisors, and the native NVENC plugin on
Windows. What newer tags add (the experimental Metal renderer in 32.x, the plugin manager,
canvases) is not needed: the engine renders with OpenGL on macOS, as OBS 31 does by default. The
prebuilt obs-deps 2024-09-12 libraries are universal (arm64 + x86_64) and built for macOS 11.0
(`minos 11.0`), so the same checkout builds the Intel and legacy engines by cross-compiling on
Apple Silicon. On Windows, OBS 31 runs on 64-bit Windows 10 and 11 and needs DirectX 11.
**One tag serves every build — no two-tag plan is needed.**

## Third-party libraries shipped with the binaries

From obs-deps 2024-09-12: FFmpeg 7 (libavcodec, libavformat, libavutil, libavdevice, libavfilter,
libswscale, libswresample; built with GPL parts), x264 (GPL-2.0+), mbedTLS (Apache-2.0),
SRT (MPL-2.0), librist (BSD-2-Clause), plus statically linked jansson, RNNoise, SpeexDSP, simde,
uthash, libvpx, opus, lame, ogg/vorbis/theora, aom, libpng (see each licence). Their licence texts
are copied into `licenses/` of every package by `scripts/package-macos.sh` /
`scripts/build-windows.ps1`.

The Windows package also carries the Microsoft Visual C++ runtime DLLs (`vcruntime140*.dll`,
`msvcp140*.dll`, `concrt140.dll`, …) next to the program — Microsoft's "app-local" deployment of
its redistributable runtime, copied from the redistributable folder of the Visual Studio that
built the engine — so the engine runs on a PC without the VC++ redistributable installed.

## Modifications log

- 2026-10-11 — first version (engine 0.1.0) against OBS 31.0.4. OBS unmodified; no patches.
  Packaging strips local symbols (`strip -x`) and re-signs the copied binaries (ad hoc); it does
  not change install names or rpaths.
- 2026-10-11 — published as https://github.com/skynatlabs/skynat-engine; the release builds
  (Apple Silicon, Intel Mac, Windows x64) are made and tested by GitHub Actions
  (`.github/workflows/build.yml`) from the tagged source. OBS still unmodified; no patches.
