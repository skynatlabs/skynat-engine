# skynat-engine pipe protocol — version 1

The app starts `skynat-engine` as a child process and talks to it only through the child's
**stdin** (app → engine) and **stdout** (engine → app). stderr carries the engine's plain-text log
(one line per message, `[level] text`) and nothing else. There is no socket, no shared memory and
no library linking between the two.

## Framing

Every message in both directions is

```
offset  size  field
0       4     payload length N, unsigned 32-bit little-endian (not counting these 5 bytes)
4       1     message type
5       N     payload
```

N is at most 64 MiB. A longer length is a protocol error: the engine stops reading and shuts down.
All integers in binary payloads are **little-endian**; `i64` is two's complement. Timestamps are
**microseconds** on the engine's clock, starting near 0 when the output starts.

| Type | Direction | Payload |
|---|---|---|
| 1 | app → engine | **command** — UTF-8 JSON object (see Commands) |
| 2 | engine → app | **event** — UTF-8 JSON object (see Events) |
| 3 | engine → app | **video config** (program) |
| 4 | engine → app | **video packet** (program) |
| 5 | engine → app | **audio config** (program) |
| 6 | engine → app | **audio packet** (program) |
| 7 | app → engine | **overlay pixels** |
| 8 | app → engine | **PCM audio** |
| 9 | engine → app | **monitor feed config** |
| 10 | engine → app | **monitor feed packet** |
| 11 | engine → app | **thumbnail** |

Unknown types sent to the engine produce an `error` event and are skipped.

### 3 — video config (program)

```
0   u16  width
2   u16  height
4   u32  fps numerator
8   u32  fps denominator
12  u32  target bitrate, kbit/s
16  …    avcC (AVCDecoderConfigurationRecord: version 1, profile, level, SPS, PPS)
```

Sent before the first video packet and again whenever the encoder's SPS/PPS change (checked on
every keyframe). This is exactly WebCodecs' `decoderConfig.description` for `avc: {format: 'avc'}`
and what an FLV AVC sequence header (`AVCPacketType 0`) carries.

### 4 — video packet (program)

```
0   i64  pts (µs)
8   i64  dts (µs) — equal to pts: the engine never uses B-frames
16  u8   1 = keyframe (IDR), 0 = not
17  …    one access unit in AVCC form: NAL units each prefixed by a 4-byte big-endian length
```

The first video packet after a config is always a keyframe. Keyframes come every `keyint_sec`
seconds (2 by default). H.264 High profile, CBR (Apple Silicon) or average bitrate with a hard
ceiling (Intel Macs), 4:2:0, BT.709 limited range. Keyframes also carry SPS/PPS in-band.

### 5 — audio config (program)

```
0   u32  sample rate (48000)
4   u16  channels (2)
6   u16  reserved (0)
8   u32  bitrate, kbit/s
12  …    AudioSpecificConfig (2 bytes for AAC-LC 48 kHz stereo: 0x11 0x90)
```

### 6 — audio packet (program)

```
0   i64  pts (µs)
8   …    one raw AAC-LC frame (1024 samples, no ADTS header)
```

Video and audio packets of the program come **interleaved in timestamp order** (libobs interleaves
them before they reach the pipe), so they can go straight into an FLV muxer: video tag
`FrameType 1|2, CodecID 7, AVCPacketType 1, CompositionTime 0` + the AVCC data; audio tag
`0xAF 0x01` + the raw frame; timestamps in ms = µs / 1000.

### 7 — overlay pixels (app → engine)

```
0   u32  overlay source id (a source added with type "overlay")
4   u32  x        ┐ rectangle inside the layer, in layer pixels
8   u32  y        │
12  u32  width    │
16  u32  height   ┘
20  u32  stride   bytes between the starts of two rows in the data (≥ width × 4)
24  u32  flags    bit 0: clear the whole layer to transparent first
28  …    RGBA, 8 bits per channel, height rows of `stride` bytes (the last row may be just width × 4)
```

Pixels are straight (not premultiplied) alpha unless the source was added with
`"premultiplied": true`; they are composited like a web page composites a canvas (sRGB space).
Send only what changed: a whole layer, or just the changed rectangle. A rectangle of size 0×0 with
the clear flag empties the layer. The engine copies the data in on its command thread and uploads
it to the GPU once on the next frame. Overlay messages are processed in order with commands, so a
layer can be sent right after its `add_source` command.

### 8 — PCM audio (app → engine)

```
0   u32  pcm source id (a source added with type "pcm")
4   u32  frame count F
8   u32  delay, µs: how long ago the first sample was captured (the app's own buffering)
12  u32  reserved (0)
16  …    F × 2 float32 samples, interleaved stereo (L R L R …), 48 kHz
```

The payload must be exactly 16 + F × 8 bytes. PCM is handled on the reader thread at once (not
queued behind commands); messages for an unknown id are counted (`pcm_unrouted` in stats) and
dropped, so add the source and wait for its result before streaming. The engine stamps each block
on its own clock (arrival time minus the block length and the delay) and keeps the stream
continuous: when the app's sound card runs a little fast or slow, it drops or repeats one sample
per block (inaudible) to stay locked; after a pause longer than 0.4 s or a drift above 200 ms it
starts a new timeline. Any block size works; 1024 frames (21.3 ms) is typical.

### 9 — monitor feed config, 10 — monitor feed packet

Same as types 3 and 4 with one extra byte in front:

```
0   u8   feed: 0 = Program monitor, 1 = Preview monitor (studio mode), 2 = the stream's own picture (start_output "stream")
1   …    type 3 / type 4 payload
```

Feeds are small hardware H.264 streams (default 854×480, 15 fps, 1000 kbit/s, Main profile,
keyframe every second) for the app's monitors — decode them with WebCodecs `VideoDecoder`.
The Program feed is a scaled copy of the program picture (nothing is rendered twice): by default
libobs scales it in its video thread; `gpu_scale: true` scales it on the GPU instead (with the
area filter when the feed is smaller than the canvas — every canvas pixel counted by how much of
it a feed pixel covers, so thin lines and small text do not shimmer; bicubic above the canvas's
size). The Preview feed's mix is made smaller the same way. Measured on an M2: while Program is also being encoded, about the
same CPU and ~2.5 ms more graphics-thread time per frame; with nothing recorded or streamed, the
GPU path spares the whole canvas's readback (engine 21.6 → 19.4 % of a core with two 1080p30
clips and both feeds), so the app asks for it. The CPU path stays the default. The Preview feed renders the preview input into its own small mix. If the pipe backs
up, feed packets are dropped first and the feed resumes at its next keyframe.

### 11 — thumbnail

```
0   u32  input source id
4   u16  width (default 256)
6   u16  height (default 144)
8   u8   format: 0 = RGBA, 1 = I420
9   u8×3 reserved
12  u32  stride (RGBA: width × 4; I420: width)
16  …    RGBA: height rows of `stride` bytes; I420: the Y plane (width × height), then U and V
         ((width/2) × (height/2) each) — full-range BT.709, each chroma sample from its 2×2 pixels
```

Each input as it looks fitted to the canvas (letterboxed), drawn on the GPU at a few frames per
second — at up to four times the thumbnail's size, then made smaller with the area filter — and
read back one frame later. Alpha is undefined — treat the picture as opaque. Dropped
first if the pipe backs up.

## Commands (type 1)

A command is a JSON object with `"cmd"` and, optionally, `"req"` (a number or string). The engine
answers **every** command with a `result` event carrying the same `req`:

```json
{"event":"result","req":7,"cmd":"add_source","ok":true,"data":{...}}
{"event":"result","req":8,"cmd":"set_program","ok":false,"error":"no input with id 9"}
```

Commands run on the engine's main thread **in the order they arrive**. Source ids are the app's
own unsigned 32-bit numbers.

| cmd | arguments | result `data` |
|---|---|---|
| `hello` | `protocol` (optional) | capabilities (same fields as `ready`) |
| `configure` | `width`, `height`, `fps` (number, e.g. 30 or 29.97) or `fps_num` + `fps_den` | the new canvas. Fails while the output or a feed runs |
| `list_devices` | — | `cameras: [{id, name}]`, `camera_permission`: `authorized` / `denied` / `restricted` / `not_determined` / `not_required` |
| `add_source` | `id`, `type`, type fields (below), `volume` (0…1+), `muted` | `{id, type}` (+ `camera: {device, fast, resolution}`) |
| `update_source` | `id` + any of the type's fields | `{id, changed}` |
| `remove_source` | `id` | `{id}` — leaves Program/Preview black if it was showing there |
| `set_program` | `id` (an input, or `null` for black), `transition`: `cut` (default) or `fade`, `ms` (default 300) | `{id, transition, ms}` |
| `set_preview` | `id` or `null` | `{id}` — what the Preview feed shows (studio mode) |
| `overlay_visible` | `id`, `visible` | `{id, visible}` |
| `media` | `id`, `action`: `play` / `pause` / `restart` / `stop` | `{id, action}` |
| `lut` | `id` (a source) or `target: "program"` (Program, under the overlays), `path` (a `.cube` or PNG look-up table; empty: take it off), `amount` (0–1, default 1) | `{target, on, amount}` — libobs's own "Apply LUT" filter (`clut_filter`); the table is the app's |
| `start_output` | `video_kbps` (default 6000), `audio_kbps` (160), `keyint_sec` (2), `encoder`: `auto` (hardware, else x264) / `hardware` / `x264` / an encoder id, `encoder_settings`: `{<encoder id>: {<libobs setting>: value}}` — the documented settings of each encoder the app wants used, applied over the engine's generic defaults (constant bitrate; x264 `veryfast`, `zerolatency`) | `{encoder, settings, hardware, audio_encoder, video_kbps, audio_kbps, keyint_sec}` — `settings`: what the encoder was opened with. Whatever is asked, the engine keeps the pipe's constraints: the bitrate and keyframe interval asked for, no B-frames, x264 `scenecut=0`. Optional `stream: {width, height, kbps}`: the stream's own smaller picture beside Program's — a second encoder of the same kind and tuning, scaled by libobs (area at half the canvas or less, bicubic above), sent as feed 2 and never dropped; answered in `stream` (`{width, height, kbps, encoder}`, or `{error}` and Program alone) |
| `stop_output` | — | `{was_running, video_packets, audio_packets}` after the encoders drained |
| `set_bitrate` | `kbps`, `target` (`program`, the default, or `stream`: the stream's own picture) | `{kbps, encoder, target}` — applies live, no restart |
| `preview_feed` | `on`, `width` (854), `height` (480), `fps` (15), `preview_fps` (= `fps`; Preview's own rate — e.g. half of Program's), `kbps` (1000), `program` (true), `preview` (true), `encoder`, `gpu_scale` (false) | `{on, width, height, fps, preview_fps, kbps, program, preview}` — sending it again with other size/rate/bitrate restarts the running feeds |
| `thumbnails` | `on`, `width` (256), `height` (144), `fps` (2), `format` (`rgba` or `i420`, default `rgba`), `only_changes` (false: when true an input's thumbnail is sent only if its picture changed since the last one sent; the first after `on` always goes) | `{on, width, height, fps, format, only_changes}` |
| `projector` | `id` (the app's number for it), `on` (true), `content`: `program` / `preview`, `fit`: `contain` / `cover` / `stretch`, `display_id` (macOS: the CGDirectDisplayID — Electron's display id), `display {x, y, width, height}` (macOS: points, top-left origin, as Electron's display bounds; Windows: physical pixels, as `screen.dipToScreenRect`), `windowed` (false; true: a plain window of exactly `display`, for checks) | `{id, on, content, fit, width, height, scale, frames, display {x, y, width, height}}` — `on: false` → `{id, on: false, was_open}` |
| `stats` | `interval_ms` (optional: 0 stops, > 0 sends `stats` events at that interval) | stats (see `stats` event) |
| `shutdown` | — | the result, then a `shutdown` event, then the process exits with code 0 |

### Projectors

A projector is a borderless window of the engine's own over a whole display (above the menu
bar and the Dock / the taskbar, on every Space, not in the window cycle), into which libobs
draws Program (the canvas it just rendered, overlays and all) or Preview (the preview input
itself, drawn at the display's size) every frame — at the display's own pixels (Retina:
`wantsBestResolutionOpenGLSurface`; Windows: the engine is per-monitor DPI aware), with nothing
encoded or decoded between the picture and the screen. Program is drawn 1:1 when the display
is the canvas's size, with the bicubic filter when it is enlarged and the area filter when it
is made smaller. It never takes the focus
when it opens; clicked, Escape or a double-click closes it (the way out when it covers the
app's own screen) — then, and when its display is unplugged, a `projector` event says so.
Sending `projector` again for an open id changes `content` and `fit` in place (another display
reopens it).

### Source types

| type | fields | notes |
|---|---|---|
| `camera` | `device` (AVFoundation `uniqueID` / DirectShow id from `list_devices`) or `device_name`; `width`, `height`, `fps` (default: the canvas); `fast` (default true); `audio` (default false) | macOS "fast" path = zero-copy IOSurface textures, the device format closest to the wish; `fast: false` uses the AVFoundation "High" preset. Camera sound is muted unless `audio: true` (the app's mixer owns sound). The camera permission belongs to the parent app |
| `media` | `path`, `loop` (true), `hw_decode` (true), `restart_on_activate` (false), `hdr` (`convert`, the default: libobs turns an HLG or PQ clip into the canvas's light; `as_is`: the coded signal unchanged — the YUV-to-RGB with the clip's own matrix only — for an app-supplied `lut` to make the picture; also on `update_source`), `rotate` (0: degrees clockwise, 0/90/180/270 — the file's own rotation, which libobs's media source does not apply; also on `update_source`) | FFmpeg media source; its sound is mixed into Program while it is on Program. Sends a `media` event when a non-looping file ends |
| `image` | `path` | PNG, JPEG, GIF, BMP, … |
| `color` | `color`: `#rrggbb` or `#rrggbbaa` | fills the canvas |
| `overlay` | `width`, `height` (default: the canvas), `x`, `y` (0), `visible` (true), `premultiplied` (false) | an RGBA layer above Program, fed with type 7 messages; later layers are on top |
| `pcm` | — | an audio input fed with type 8 messages; always mixed into Program |

Inputs (`camera`, `media`, `image`, `color`) are each fitted to the canvas (scaled to fit, centred,
letterboxed) and can be put on Program, Preview, the feeds and thumbnails. Overlays always sit on
top of Program.

## Events (type 2)

| event | fields |
|---|---|
| `ready` | sent once after start-up, before any command is read: `engine`, `version`, `protocol`, `libobs`, `renderer`, `pid`, `video {width, height, fps_num, fps_den, format, colorspace, range}`, `audio {sample_rate, channels}`, `encoders {h264_hardware: [ids], aac: [ids], x264}`, `sources` |
| `result` | `req`, `cmd`, `ok`, `data` or `error` |
| `stats` | `render_ms` (average time libobs spent rendering a frame), `fps`, `frames_rendered`, `frames_lagged`, `frames_output`, `frames_skipped` (encoder could not keep up), `output {active, encoder, kbps, frames, frames_dropped, video_packets, audio_packets, keyframes}`, `feeds {program, preview}`, `pcm [{id, frames, offset_ms, inserted, dropped, resyncs}]`, `pcm_unrouted`, `overlay_updates`, `thumbnails_sent`, `thumbnails_unchanged` (not sent: the same as the last), `cpu_percent` (of one core, since the previous stats), `threads`, `memory_mb`, `pipe {queued_bytes, bytes_written, bulk_dropped, media_dropped}`, `projectors [{id, content, fit, width, height, frames, …}]`, `uptime_s`, `sources`, `scaled {bicubic, area, lanczos}` (cameras and clips scaled to the canvas — enlarged with libobs's bicubic, made smaller with its Lanczos; a source turned a quarter by `rotate` measured by its turned size — checked twice a second from their own size) |
| `output` | `name` (`program`, `feed-program`, `feed-preview`), `state` (`started` / `stopped`), `code`, `error` |
| `media` | `id`, `state: "ended"` |
| `projector` | `id`, `state: "closed"`, `reason`: `escape` / `double-click` / `closed` / `display-gone` — the operator closed it at the projector, or its display went away (the engine has closed it already) |
| `log` | `level` (`error` / `warning`), `message` — at most 20 per second; the full log is on stderr |
| `error` | `message`, `fatal` (true only when start-up failed; the process then exits with code 3) |
| `shutdown` | `reason` — the last event before the process exits |

## Lifetime

- The engine exits with code 0 when its **stdin closes** (the parent quit or crashed), on the
  `shutdown` command, on SIGTERM/SIGINT/SIGHUP (macOS) or when its stdout breaks. A watchdog ends
  the process if shutting down takes longer than 8 seconds.
- Start-up failure (no graphics, libobs missing): `error` event with `fatal: true`, exit code 3.
  Bad arguments: exit code 64.
- `--no-video` (diagnostics, never used by the app): the engine starts without a renderer, so it
  also starts on a computer without a usable GPU. `ready` and `hello` then carry `"video": null`
  and `"renderer": "none"`; `hello`, `list_devices`, `stats` and `shutdown` work, every other
  command is refused with `video is off (the engine was started with --no-video)`.
- After a crash or restart the engine starts empty: the app sends its sources and settings again
  when it sees the new `ready` event.

## Threads

The engine adds three threads of its own to libobs's: the **stdin reader**, the **stdout writer**
and the **main thread** (macOS run loop / Windows message loop), where commands run. libobs runs
its graphics thread, one video-output thread per mix, the audio thread, the hotkey thread and a
task thread; the media source, AVFoundation, VideoToolbox and CoreAudio add their own system
threads. Measured counts are in the test report.
