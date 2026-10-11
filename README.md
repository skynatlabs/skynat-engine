# skynat-engine

A silent, headless live-production engine **built from OBS Studio's libobs**.

The Skynat Live app starts it as a hidden child process. It has no window, no Dock icon, no tray
icon and no interface of its own: it captures cameras and media files, composites them, encodes
with the computer's hardware encoder and keeps the audio/video clock, and it talks to the app only
through its standard input and output (a small framed binary protocol, see
[`PROTOCOL.md`](PROTOCOL.md)). The app keeps the interface, overlays and features, and sends
overlays as pictures and its audio mix as PCM.

- **Licence:** GNU General Public License, version 2 or (at your option) any later version — see
  [`COPYING`](COPYING). It is a separate program from Skynat Live; the app never links or loads
  libobs and only exchanges messages with this process over pipes.
- **Built from:** OBS Studio **31.0.4**, unmodified, and the OBS project's prebuilt dependencies.
  Why that tag, what is used and what is switched off: [`NOTICE.md`](NOTICE.md).

## What it does

| | |
|---|---|
| Sources | cameras (AVFoundation zero-copy on macOS, DirectShow on Windows), media files (FFmpeg, hardware decoding, looping), images, solid colours, RGBA overlay layers fed over the pipe, and an audio input fed with the app's mixer (float32 stereo 48 kHz) |
| Switching | Program with cut or fade, a studio-mode Preview, overlays on top of Program |
| Program output | hardware H.264 (VideoToolbox; NVENC / Quick Sync / AMF on Windows), CBR, 2 s keyframes, High profile, no B-frames, + AAC-LC — written to stdout as avcC/AudioSpecificConfig and AVCC/raw-AAC packets, ready for an FLV muxer; bitrate can change live |
| Monitor feeds | small hardware-encoded H.264 copies of Program and Preview (default 854×480 at 15 fps) for the app's monitors |
| Thumbnails | 256×144 RGBA pictures of every input a few times per second for source tiles and multiview |
| Stats | average render time, lagged/skipped frames, encoder frames, CPU, thread count, memory |

Software x264 is used only when no hardware H.264 encoder exists (or when asked for).

## Layout of a build

```
dist/darwin-arm64/                 dist/win32-x64/
  bin/skynat-engine                  skynat-engine.exe, obs.dll, libobs-d3d11.dll, FFmpeg DLLs …
  Frameworks/libobs.framework …      obs-plugins/64bit/*.dll
  PlugIns/*.plugin                   data/libobs/, data/obs-plugins/<module>/
  COPYING NOTICE.md SOURCE.txt licenses/   COPYING NOTICE.md SOURCE.txt licenses/
```

Copy the folder anywhere (for the app: `Contents/Resources/engine/`) and run
`bin/skynat-engine` (macOS) or `skynat-engine.exe` (Windows). Options: `--width`, `--height`,
`--fps`, `--plugins`, `--data`, `--config`, `--renderer`, `--log-level`, `--no-video`
(diagnostics: the pipe without a renderer), `--version`. It exits when its standard input closes,
on the `shutdown` command, or on SIGTERM.

## Building

### macOS (Apple Silicon or Intel host; Xcode 15.1+ and CMake 3.28+)

```sh
scripts/build-macos.sh arm64 11.0     # Apple Silicon
scripts/build-macos.sh x86_64 11.0    # Intel and older Macs (cross-compiles on Apple Silicon)
```

The script clones OBS Studio 31.0.4 into `../skynat-engine-work/obs-studio` (override with
`SKYNAT_ENGINE_WORK`), lets OBS's own CMake download its prebuilt dependencies, configures the
headless build with the Xcode generator, builds libobs, the OpenGL renderer and the plugins the
engine uses, builds the engine against that libobs, and packages everything into
`dist/darwin-<arch>/` (`scripts/package-macos.sh`).

### Windows (Visual Studio 2022, CMake 3.28+, PowerShell 7)

```powershell
pwsh scripts/build-windows.ps1            # → dist/win32-x64/
```

Same steps with OBS's `windows-x64` preset; written for GitHub Actions `windows-2022` runners.

## Tests

Node.js 22.18 or newer (it runs the TypeScript client directly) and ffmpeg (on `PATH`, or
`FFMPEG=/path/to/ffmpeg`):

```sh
node tests/framing.test.mjs          # protocol framing (no engine needed)
node tests/engine.test.mjs           # starts dist/<platform>/… and checks everything
```

`tests/engine.test.mjs` makes a 10-second 1080p30 test clip with ffmpeg, plays it in a loop with an
overlay and a PCM tone, records the program packets for ~10 s into an FLV file and probes it, then
checks keyframes, timestamps, frame rate, audio continuity, cut/fade switching, the monitor feeds,
thumbnails, a live bitrate change, a crash restart, that the engine exits when its stdin closes,
and the pipe without a renderer (`--no-video`). It also prints the engine's CPU use, render time
and thread count. On a real computer it requires the hardware H.264 encoder.

## Continuous integration and releases

Every push to `main` builds and tests the engine on GitHub Actions
([`.github/workflows/build.yml`](.github/workflows/build.yml)):

| Job | Runner | Build | Tests |
|---|---|---|---|
| Apple Silicon | `macos-15` (arm64) | `scripts/build-macos.sh arm64 11.0` | framing + end to end |
| Intel Mac | `macos-15-intel` (x86_64) | `scripts/build-macos.sh x86_64 11.0` | framing + end to end, natively on Intel |
| Windows x64 | `windows-2022` | `scripts/build-windows.ps1 -Smoke` | framing + end to end |

The runners are virtual machines without a GPU or a hardware video encoder. There — and only
there (`SKYNAT_ENGINE_TEST_SOFTWARE=1` is honoured only when `CI=true`) — the end-to-end test
accepts the program on the software x264 fallback; because libobs renders on a software
rasterizer (Apple Software Renderer, Microsoft Basic Render Driver) it runs on a 480x270 canvas
at 30 fps with the clip decoded in software; and if the rasterizer still cannot keep real time
(GitHub's macOS runners take 40–90 ms a frame even then), the checks that need real time are
reported, not enforced, and the log says so in capitals. Every other check is exactly as strict
as on a real computer, where 1080p30, real time and hardware H.264 are all required. A runner
whose graphics cannot start at all would be reported in plain words and checked without video.
Pushing a tag `v*` attaches the three tested packages to that tag's release, with what each
runner proved: `skynat-engine-darwin-arm64.zip`, `skynat-engine-darwin-x86_64.zip` (macOS 11.0
or newer; the frameworks' symlinks are kept — unpack with Finder, `ditto -x -k` or `unzip`) and
`skynat-engine-win32-x64.zip` (64-bit Windows 10/11 with DirectX 11).

## App side

[`client/engine-client.ts`](client/engine-client.ts) is a dependency-free TypeScript client
(Node's `child_process` + `Buffer` only): it starts the engine hidden, frames and parses messages,
offers typed commands and events, restarts the engine with back-off if it crashes, and cleans up
orphans. It contains no OBS code and only speaks the pipe protocol, so it is **MIT-licensed**
(stated in the file) and can be copied into a closed-source app; everything else in this
repository is GPL-2.0-or-later.

## Source

https://github.com/skynatlabs/skynat-engine — Copyright © 2026 Skynat.
