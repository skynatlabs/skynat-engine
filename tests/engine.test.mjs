// Integration test: starts the packaged engine and checks the whole pipe.
//
//   node tests/engine.test.mjs
//
// Environment:
//   SKYNAT_ENGINE   engine executable (default dist/darwin-<arch>/bin/skynat-engine,
//                   Windows dist/win32-x64/skynat-engine.exe)
//   FFMPEG          ffmpeg binary (default: ffmpeg on PATH)
//   SKYNAT_TEST_CAMERA=1  also opens the first camera (needs camera permission)
//   SKYNAT_ENGINE_TEST_SOFTWARE=1  CI only (honoured only when CI=true, as on GitHub
//                   Actions). The runners are virtual machines:
//                   - without a hardware video encoder the program may use the software
//                     x264 fallback;
//                   - without a GPU, libobs renders on a software rasterizer (Apple
//                     Software Renderer, Microsoft Basic Render Driver), which cannot draw
//                     1080p in real time: the checks then run on a 480x270 canvas at
//                     30 fps (positions and sizes scale with it) and the clip is decoded
//                     in software;
//                   - if even that rasterizer lags (GitHub's macOS runners: 40-90 ms a
//                     frame), the checks that need real time (frame rate, gaps, keyframe
//                     spacing, bitrate, audio cadence, timing of cut and fade, feed rate,
//                     the clip's own sound) are reported but not enforced, and the log
//                     says so in capitals;
//                   - if the renderer cannot start at all, the test says so plainly and
//                     checks the pipe without video (--no-video) instead.
//                   Every other check stays exactly as strict as on a real computer.
//                   Local runs ignore the variable: 1080p30, real time and hardware
//                   H.264 are all required.
//
// Run it at normal priority (performance numbers are measured) and alone.
// SPDX-License-Identifier: GPL-2.0-or-later

import { spawn, spawnSync, execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, writeFileSync, statSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { EngineClient } from '../client/engine-client.ts';
import { FlvWriter } from './lib/flv.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const ARCH = process.arch === 'arm64' ? 'arm64' : 'x86_64';
const PLATFORM_DIR = process.platform === 'win32' ? 'win32-x64' : `darwin-${ARCH}`;
const ENGINE = process.env.SKYNAT_ENGINE || (process.platform === 'win32'
  ? join(ROOT, 'dist', PLATFORM_DIR, 'skynat-engine.exe')
  : join(ROOT, 'dist', PLATFORM_DIR, 'bin', 'skynat-engine'));
const OUT_DIR = join(ROOT, 'tests', 'results');
mkdirSync(OUT_DIR, { recursive: true });

const CI = process.env.CI === 'true';
// Every stderr line of the engine sessions; saved as tests/results/engine.log
// (and its end printed when a check fails in CI).
const engineLog = [];
const SOFTWARE_OK = process.env.SKYNAT_ENGINE_TEST_SOFTWARE === '1' && CI;
if (process.env.SKYNAT_ENGINE_TEST_SOFTWARE === '1' && !CI) {
  console.log('NOTE  SKYNAT_ENGINE_TEST_SOFTWARE is ignored outside CI: this run requires hardware H.264.\n');
}
// What the run proved, for the summary (last-run.json).
let mode = 'full';            // 'full', or 'no-gpu' (the renderer could not start at all)
let softwareEncoder = false;  // CI runner without a hardware H.264 encoder: x264 accepted
let softwareRenderer = false; // CI runner without a GPU: a CPU rasterizer, small canvas
let excusedRealtime = false;  // CI CPU rasterizer that lagged: real-time checks reported only
const realtimeNotes = [];     // those checks and whether they held
let adapter = '';             // the graphics adapter libobs rendered on
let encoderUsed = '';         // the program's video encoder
// The canvas the checks run at: 1920x1080, or SMALL on a CI runner whose
// "GPU" is a software rasterizer (it cannot draw 1080p in real time).
const SMALL = { w: 480, h: 270 };
let W = 1920, H = 1080;

function findFfmpeg() {
  const candidates = [process.env.FFMPEG];
  for (const c of candidates) if (c && existsSync(c)) return c;
  return 'ffmpeg';
}
const FFMPEG = findFfmpeg();

// ---------------------------------------------------------------------------
const results = [];
function check(name, ok, detail = '') {
  results.push({ name, ok: !!ok, detail: String(detail) });
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail !== '' ? `  — ${detail}` : ''}`);
}
function note(name, detail) {
  results.push({ name, ok: true, note: true, detail: String(detail) });
  console.log(`INFO  ${name}  — ${detail}`);
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function ffmpeg(args, opts = {}) {
  const r = spawnSync(FFMPEG, ['-hide_banner', ...args], { maxBuffer: 512 * 1024 * 1024, ...opts });
  return { code: r.status, stdout: r.stdout, stderr: r.stderr ? r.stderr.toString() : '' };
}

/** cumulative CPU seconds of a process (ps "time" column). */
function cpuSeconds(pid) {
  if (process.platform === 'win32') return NaN;
  const out = execFileSync('ps', ['-o', 'time=', '-p', String(pid)]).toString().trim();
  const parts = out.split(':').map(Number);
  return parts.length === 3 ? parts[0] * 3600 + parts[1] * 60 + parts[2] : parts[0] * 60 + parts[1];
}
function threadCount(pid) {
  if (process.platform !== 'darwin') return NaN;
  const out = execFileSync('ps', ['-M', '-p', String(pid)]).toString().trim().split('\n');
  return out.length - 1;
}

function rgbAt(frame, width, x, y) {
  const o = (y * width + x) * 3;
  return [frame[o], frame[o + 1], frame[o + 2]];
}
function decodeFrame(file, seconds, width, height) {
  const r = ffmpeg(['-v', 'error', '-ss', seconds.toFixed(3), '-i', file, '-frames:v', '1', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-']);
  return r.stdout && r.stdout.length === width * height * 3 ? r.stdout : null;
}
/** Mean distance of a frame from pure blue (0..441). */
function blueDistance(frame) {
  let sum = 0, n = 0;
  for (let i = 0; i < frame.length; i += 3 * 97) {
    const dr = frame[i], dg = frame[i + 1], db = 255 - frame[i + 2];
    sum += Math.sqrt(dr * dr + dg * dg + db * db); n++;
  }
  return sum / n;
}
/** Mean distance from pure blue of the video, sampled `rate` times a second on tiny frames. */
function blueTimeline(file, rate = 10) {
  const w = 32, h = 18, size = w * h * 3;
  const r = ffmpeg(['-v', 'error', '-i', file, '-an', '-vf', `fps=${rate},scale=${w}:${h}:flags=area`, '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-']);
  const out = [];
  if (!r.stdout) return out;
  for (let i = 0; (i + 1) * size <= r.stdout.length; i++) {
    let sum = 0;
    for (let o = i * size; o < (i + 1) * size; o += 3) {
      const dr = r.stdout[o], dg = r.stdout[o + 1], db = 255 - r.stdout[o + 2];
      sum += Math.sqrt(dr * dr + dg * dg + db * db);
    }
    out.push({ t: i / rate, d: sum / (w * h) });
  }
  return out;
}
/**
 * Level of a tone: the Goertzel amplitude over 0.25 s windows (4 Hz wide bins),
 * averaged. One long window would be a fraction of a hertz wide, and the
 * engine's clock steering (one sample dropped or repeated per block when the
 * app's sound card drifts) moves a tone by about a hertz.
 */
function toneLevel(samples, rate, freq) {
  const n = Math.floor(rate / 4);
  if (samples.length < n) return goertzel(samples, rate, freq);
  let sum = 0, count = 0;
  for (let o = 0; o + n <= samples.length; o += n) { sum += goertzel(samples.subarray(o, o + n), rate, freq); count++; }
  return sum / count;
}
function goertzel(samples, rate, freq) {
  const k = Math.round((samples.length * freq) / rate);
  const w = (2 * Math.PI * k) / samples.length;
  const c = 2 * Math.cos(w);
  let s0 = 0, s1 = 0, s2 = 0;
  for (let i = 0; i < samples.length; i++) { s0 = samples[i] + c * s1 - s2; s2 = s1; s1 = s0; }
  return Math.sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / (samples.length / 2);
}

// ---------------------------------------------------------------------------
async function main() {
  console.log(`engine: ${ENGINE}\nffmpeg: ${FFMPEG}\n`);
  check('engine executable exists', existsSync(ENGINE), ENGINE);
  if (!existsSync(ENGINE)) return;

  // 1. Main session (the test clip is made once the canvas size is known).
  const log = engineLog;
  // On runners the engine logs at debug level (libobs's own audio/timing notes), for the saved log.
  const client = new EngineClient({ executable: ENGINE, args: ['--log-level', CI ? 'debug' : 'info'], restart: false, startTimeoutMs: 20000, onLog: (l) => log.push(l) });
  const startEvents = [];
  const onStartEvent = (ev) => startEvents.push(ev);
  client.on('event', onStartEvent);
  let startExit = null;
  client.on('exit', (info) => { if (!startExit) startExit = info; });
  const t0 = Date.now();
  let ready;
  try {
    ready = await client.start();
  } catch (e) {
    for (let i = 0; i < 40 && !startExit; i++) await sleep(50);
    const fatal = startEvents.find((ev) => ev.event === 'error' && ev.fatal);
    // A runner without any usable GPU: the renderer cannot start (a missing
    // graphics module is a packaging fault, never accepted here).
    if (SOFTWARE_OK && fatal && /graphics not supported|obs_reset_video failed/.test(fatal.message)) {
      mode = 'no-gpu';
      console.log('\n' + '='.repeat(78));
      console.log('VIDEO CANNOT START ON THIS RUNNER: it has no usable GPU (the engine could not');
      console.log(`create its ${process.platform === 'win32' ? 'Direct3D 11' : 'OpenGL'} renderer: "${fatal.message}").`);
      console.log('The video, encoding, switching, feed and thumbnail checks need a GPU and were');
      console.log('NOT run here. Checking the pipe without video (--no-video) instead.');
      console.log('='.repeat(78) + '\n');
      check('without a GPU the engine stops at start-up with a readable reason (exit 3)', startExit && startExit.code === 3,
        `${fatal.message}; exit ${startExit ? startExit.code : 'not seen'}`);
      await noVideoChecks();
      return;
    }
    throw new Error(`engine did not start: ${fatal ? fatal.message : e.message}`);
  }
  client.off('event', onStartEvent);
  const readyMs = Date.now() - t0;
  if (CI) {
    // On runners: the engine's own figures once a second, into the saved log.
    client.on('event', (ev) => {
      if (ev.event === 'stats') {
        log.push(`[stats] render ${Number(ev.render_ms).toFixed(1)} ms, rendered ${ev.frames_rendered}, lagged ${ev.frames_lagged}, skipped ${ev.frames_skipped}, cpu ${Number(ev.cpu_percent).toFixed(0)} %, output ${JSON.stringify(ev.output)}, feeds ${JSON.stringify(ev.feeds)}, pipe ${JSON.stringify(ev.pipe)}`);
      } else if (ev.event !== 'result') {
        log.push(`[event] ${JSON.stringify(ev)}`);
      }
    });
  }
  check('ready event', ready.event === 'ready' && ready.protocol === 1 && ready.video.width === 1920 && ready.video.height === 1080,
    `${readyMs} ms, libobs ${ready.libobs}, renderer ${ready.renderer}`);
  const hw = ready.encoders.h264_hardware;
  if (hw.length > 0 || !SOFTWARE_OK) {
    check('hardware H.264 encoder found', hw.length > 0, hw.join(', '));
  } else {
    softwareEncoder = true;
    note('hardware H.264 encoder', 'none on this CI runner (a virtual machine): the software x264 fallback is accepted here (SKYNAT_ENGINE_TEST_SOFTWARE=1); every other check is unchanged');
    check('software x264 fallback available', ready.encoders.x264 === true);
  }
  check('AAC encoder found', ready.encoders.aac.length > 0, ready.encoders.aac.join(', '));
  const pid = ready.pid;
  note('threads at idle', `${threadCount(pid)} (ps -M)`);

  const hello = await client.hello();
  check('hello answers with capabilities', hello.engine === 'skynat-engine' && hello.protocol === 1, `version ${hello.version}`);
  if (CI) await client.stats(1000);

  // The adapter libobs rendered on (its log says "Loading up OpenGL|D3D11 on adapter …").
  const adapterLine = (log.find((l) => /Loading up (OpenGL|D3D11) on adapter/.test(l)) || '').trim(); // Windows lines end in \r
  adapter = (adapterLine.match(/on adapter (.+?)(?: \(\d+\))?$/) || [])[1] || 'unknown';
  const cpuRenderer = /Software Renderer|Basic Render Driver/i.test(adapter);
  note('graphics adapter', adapter);
  if ((cpuRenderer && SOFTWARE_OK) || process.env.SKYNAT_ENGINE_TEST_SMALL === "1") {
    // A virtual machine without a GPU: the "GPU" is a software rasterizer on the
    // CPU (about 200 ms for one 1080p frame on GitHub's macOS runners), so
    // nothing could run in real time at 1080p. The checks run on a small canvas
    // the CPU can render 30 times a second instead — every check unchanged.
    softwareRenderer = true;
    W = SMALL.w; H = SMALL.h;
    console.log('\n' + '='.repeat(78));
    console.log(`THIS RUNNER HAS NO GPU: libobs renders on "${adapter}", a software`);
    console.log(`rasterizer on the CPU, which cannot draw 1920x1080 30 times a second. The`);
    console.log(`end-to-end checks therefore run on a ${W}x${H} canvas at 30 fps; every check is`);
    console.log(`otherwise unchanged. 1080p real-time behaviour is proven on real hardware.`);
    console.log('='.repeat(78) + '\n');
    const c = await client.configure({ width: W, height: H, fps: 30 });
    check(`configure: the ${W}x${H} canvas for this runner`, c.width === W && c.height === H && c.fps_num === 30, JSON.stringify(c));
  }
  const k = W / 1920; // positions below are designed on 1920x1080 and scaled to the canvas
  const sc = (v) => Math.round(v * k);

  // 2. Test clip: 10 s at the canvas size, 30 fps, H.264 + 440 Hz AAC.
  const clip = join(OUT_DIR, `clip-${W}x${H}p30.mp4`);
  if (!existsSync(clip)) {
    const r = ffmpeg(['-loglevel', 'error', '-y', '-f', 'lavfi', '-i', `testsrc2=size=${W}x${H}:rate=30`, '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
      '-t', '10', '-c:v', 'libx264', '-preset', 'veryfast', '-pix_fmt', 'yuv420p', '-g', '30', '-c:a', 'aac', '-b:a', '128k', '-shortest', clip]);
    check('test clip generated with ffmpeg', r.code === 0 && existsSync(clip), r.stderr.slice(0, 300));
  } else {
    check('test clip present', true, clip);
  }

  const devices = await client.listDevices();
  note('cameras', `${devices.cameras.length} found, permission ${devices.camera_permission}: ${devices.cameras.map((c) => `${c.name}${c.best_format ? ` (${c.best_format.width}x${c.best_format.height}@${c.best_format.fps})` : ''}`).join(' | ')}`);

  let unknownOk = false;
  try { await client.command('no_such_command'); } catch (e) { unknownOk = /unknown command/.test(e.message); }
  check('unknown command is answered with an error result', unknownOk);

  // Overlay layers, designed on 1080p: a red 400x200 at (100, 100) and a 50 % white
  // 200x100 at (1200, 600); scaled with the canvas.
  const ov1 = { x: sc(100), y: sc(100), w: sc(400), h: sc(200) & ~1 };
  const ov2 = { x: sc(1200), y: sc(600), w: sc(200), h: sc(100) };
  // A CPU-rendered CI runner is a virtual machine without a media engine: the
  // clip is decoded in software there (with the VM's paravirtualized decoder
  // the clip's sound went missing on GitHub's arm64 runners).
  await client.addSource({ id: 1, type: 'media', path: clip, loop: true, ...(softwareRenderer ? { hw_decode: false } : {}) });
  if (softwareRenderer) note('clip decoding', 'software (hw_decode: false) on this CPU-rendered runner');
  await client.addSource({ id: 2, type: 'color', color: '#0000ff' });
  await client.addSource({ id: 10, type: 'overlay', width: ov1.w, height: ov1.h, x: ov1.x, y: ov1.y });
  await client.addSource({ id: 20, type: 'pcm' });
  // An image input (magenta PNG made with ffmpeg) — checked through its thumbnail.
  const png = join(OUT_DIR, 'magenta.png');
  if (!existsSync(png)) ffmpeg(['-loglevel', 'error', '-y', '-f', 'lavfi', '-i', 'color=c=0xff00ff:s=320x180', '-frames:v', '1', png]);
  await client.addSource({ id: 4, type: 'image', path: png });
  let camOk = null;
  if (process.env.SKYNAT_TEST_CAMERA === '1' && devices.cameras.length) {
    try { const r = await client.addSource({ id: 3, type: 'camera', device: devices.cameras[0].id }); camOk = r; } catch (e) { camOk = e.message; }
    note('camera source', JSON.stringify(camOk));
  }

  // Overlay: a solid red layer, sent in two halves to exercise rectangles.
  const red = Buffer.alloc(ov1.w * ov1.h * 4);
  for (let i = 0; i < red.length; i += 4) { red[i] = 255; red[i + 1] = 0; red[i + 2] = 0; red[i + 3] = 255; }
  const halfRows = ov1.h / 2;
  client.sendOverlay(10, 0, 0, ov1.w, halfRows, ov1.w * 4, red, true);
  client.sendOverlay(10, 0, halfRows, ov1.w, halfRows, ov1.w * 4, red.subarray(ov1.w * halfRows * 4));
  // A second layer: 50 % white, straight alpha.
  await client.addSource({ id: 11, type: 'overlay', width: ov2.w, height: ov2.h, x: ov2.x, y: ov2.y });
  const half = Buffer.alloc(ov2.w * ov2.h * 4);
  for (let i = 0; i < half.length; i += 4) { half[i] = 255; half[i + 1] = 255; half[i + 2] = 255; half[i + 3] = 128; }
  client.sendOverlay(11, 0, 0, ov2.w, ov2.h, ov2.w * 4, half);

  await client.setProgram(1, 'cut');
  await client.setPreview(2);

  // PCM: a 1 kHz tone from "the app's mixer", in real time, 1024-frame blocks.
  let pcmFrames = 0;
  const pcmStart = Date.now();
  const pcmTimer = setInterval(() => {
    const due = Math.floor(((Date.now() - pcmStart) / 1000) * 48000);
    while (pcmFrames + 1024 <= due) {
      const block = new Float32Array(2048);
      for (let i = 0; i < 1024; i++) { const v = 0.3 * Math.sin((2 * Math.PI * 1000 * (pcmFrames + i)) / 48000); block[2 * i] = v; block[2 * i + 1] = v; }
      client.sendPcm(20, block, 21333);
      pcmFrames += 1024;
    }
  }, 10);
  await sleep(300);

  // Recorders.
  const flv = new FlvWriter();
  const feedFlv = [new FlvWriter({ audio: false }), new FlvWriter({ audio: false })];
  const video = [], audio = [], feeds = [[], []], feedCfg = [null, null], thumbs = new Map();
  let vcfg = null, acfg = null, firstPacketAt = 0;
  const events = [];
  client.on('event', (ev) => { if (ev.event !== 'result') events.push({ at: Date.now(), ev }); });
  client.on('videoConfig', (c) => { vcfg = c; flv.videoConfig(c); });
  client.on('audioConfig', (c) => { acfg = c; flv.audioConfig(c); });
  client.on('videoPacket', (p) => { if (!firstPacketAt) firstPacketAt = Date.now(); video.push({ ...p, at: Date.now(), size: p.data.length, nal: nalTypes(p.data) }); flv.videoPacket(p); });
  client.on('audioPacket', (p) => { audio.push({ ptsUs: p.ptsUs, size: p.data.length }); flv.audioPacket(p); });
  client.on('feedConfig', (c) => { feedCfg[c.feed] = c; feedFlv[c.feed].videoConfig(c); });
  client.on('feedPacket', (p) => { feeds[p.feed].push({ ptsUs: p.ptsUs, keyframe: p.keyframe, size: p.data.length }); feedFlv[p.feed].videoPacket(p); });
  client.on('thumbnail', (t) => { const l = thumbs.get(t.id) || []; l.push({ w: t.width, h: t.height, len: t.data.length, px: [...t.data.subarray((72 * t.width + 128) * 4, (72 * t.width + 128) * 4 + 4)] }); thumbs.set(t.id, l); });

  const out = await client.startOutput({ video_kbps: 6000, audio_kbps: 160 });
  encoderUsed = out.encoder;
  if (out.hardware === true || !SOFTWARE_OK) {
    check('output started on the hardware encoder', out.hardware === true, `${out.encoder} + ${out.audio_encoder}`);
  } else {
    softwareEncoder = true;
    check('output started on the software x264 fallback (CI runner without a hardware encoder)', out.encoder === 'obs_x264',
      `${out.encoder} + ${out.audio_encoder}`);
  }
  check('the output says what its encoder was opened with', !!out.settings && out.settings.bitrate === 6000 && out.settings.keyint_sec === 2, `${out.encoder}: ${JSON.stringify(out.settings).slice(0, 160)}`);
  const tOut = Date.now();
  const at = (s) => sleep(Math.max(0, tOut + s * 1000 - Date.now()));

  await client.previewFeed({ on: true, width: 854, height: 480, fps: 15, preview_fps: 10, kbps: 800 });
  await client.thumbnails({ on: true, width: 256, height: 144, fps: 4 });

  // Steady-state measurement window: t = 1 s .. 3.9 s and 6.5 .. 9.5 s.
  await at(1.0);
  const st1 = await client.stats();
  const cpu1 = cpuSeconds(pid); const w1 = Date.now();
  const cutAt = 4.0, fadeAt = 6.0, rateAt = 10.0, stopAt = 16.0;
  await at(cutAt); const cutWall = Date.now(); await client.setProgram(2, 'cut');
  await at(fadeAt); const fadeWall = Date.now(); await client.setProgram(1, 'fade', 1000);
  await at(9.5);
  const st = await client.stats();
  const cpu2 = cpuSeconds(pid); const w2 = Date.now();
  const threadsActive = threadCount(pid);
  await at(rateAt); const rateWall = Date.now(); await client.setBitrate(2000);
  await at(stopAt);
  // Stop once the recording holds the whole schedule. In real time it already
  // does; on a runner that cannot keep real time the packets can trail the
  // commands by many seconds (libobs repeats late frames, so their timestamps
  // still follow the clock) — GitHub's Intel Macs had received only 6.2 s of
  // picture after 16 s.
  for (const until = Date.now() + 60000; Date.now() < until; await sleep(50)) {
    if (video.length && (video[video.length - 1].ptsUs - video[0].ptsUs) / 1e6 >= stopAt - 0.2) break;
  }
  const stopRes = await client.stopOutput();
  await client.previewFeed({ on: false });
  await client.thumbnails({ on: false });
  clearInterval(pcmTimer);
  const finalStats = await client.stats();

  if (video.length < 30 || audio.length < 30) {
    // Without program packets nothing below can be measured: say what the engine saw and stop.
    check('program packets arrived', false, `${video.length} video / ${audio.length} audio packets; at 9.5 s the engine reported ${JSON.stringify({
      render_ms: st.render_ms, frames_rendered: st.frames_rendered, frames_lagged: st.frames_lagged, frames_skipped: st.frames_skipped, output: st.output })}; events: ${JSON.stringify(events.map((e) => e.ev).filter((e) => e.event !== 'stats').slice(0, 12))}`);
    throw new Error('no program packets arrived (see the engine log)');
  }

  // Real time: the steady-state window had no lagged or skipped frames. A CI
  // runner that renders on a CPU rasterizer may not manage that (GitHub's macOS
  // runners: 40-90 ms per frame even at 480x270); there, and only there, the
  // checks that need real time are reported but not enforced — and the log says
  // so in capitals. Everything else is checked as strictly as anywhere.
  const laggedSteady = st.frames_lagged - st1.frames_lagged, skippedSteady = st.frames_skipped - st1.frames_skipped;
  excusedRealtime = SOFTWARE_OK && softwareRenderer && (laggedSteady > 0 || skippedSteady > 0);
  if (excusedRealtime) {
    console.log('\n' + '='.repeat(78));
    console.log(`THIS RUNNER CANNOT RENDER IN REAL TIME: ${laggedSteady} frames lagged and ${skippedSteady} skipped`);
    console.log(`in the steady-state window (average ${st.render_ms.toFixed(1)} ms per frame on "${adapter}").`);
    console.log('The checks that need real time (frame rate, gaps, keyframe spacing, bitrate,');
    console.log('audio cadence, timing of cut and fade, feed rate, the clip\'s own sound) are');
    console.log('REPORTED, NOT ENFORCED here.');
    console.log('Every other check is enforced. Real time is proven on real hardware.');
    console.log('='.repeat(78) + '\n');
  }
  // A check that needs real time: enforced, unless this runner cannot keep it.
  const rt = (name, ok, detail = '') => {
    if (!excusedRealtime) return check(name, ok, detail);
    realtimeNotes.push({ name, ok: !!ok });
    note(`${name} [real time: not enforced on this runner]`, `${ok ? 'holds' : 'DOES NOT HOLD'}${detail !== '' ? ` — ${detail}` : ''}`);
  };

  // ---- program stream -------------------------------------------------------
  check('avcC received before video', vcfg && vcfg.avcC[0] === 1 && vcfg.width === W && vcfg.height === H,
    vcfg ? `${vcfg.width}x${vcfg.height}, profile 0x${vcfg.avcC[1].toString(16)} level ${vcfg.avcC[3] / 10}, ${vcfg.fpsNum}/${vcfg.fpsDen} fps, ${vcfg.kbps} kbps` : 'none');
  check('H.264 High profile', vcfg && vcfg.avcC[1] === 100);
  check('AudioSpecificConfig received (AAC-LC 48 kHz stereo)', acfg && acfg.sampleRate === 48000 && acfg.channels === 2 && ((acfg.asc[0] >> 3) === 2),
    acfg ? acfg.asc.toString('hex') : 'none');
  check('first video packet is a keyframe', video.length > 0 && video[0].keyframe);

  const win = video.filter((p) => p.ptsUs <= 10e6);
  const keys = video.filter((p) => p.keyframe).map((p) => p.ptsUs / 1e6);
  const keyGaps = keys.slice(1).map((k, i) => k - keys[i]);
  rt('keyframes every ~2 s', keyGaps.length >= 3 && keyGaps.every((g) => Math.abs(g - 2) < 0.07),
    keys.map((k) => k.toFixed(2)).join(', '));
  let mono = true, dtsEq = true;
  for (let i = 1; i < video.length; i++) { if (video[i].ptsUs <= video[i - 1].ptsUs) mono = false; }
  for (const p of video) if (p.dtsUs !== p.ptsUs) dtsEq = false;
  check('video timestamps strictly increasing', mono);
  check('no B-frames (dts == pts)', dtsEq);
  const span = (win[win.length - 1].ptsUs - win[0].ptsUs) / 1e6;
  const fps = (win.length - 1) / span;
  rt('~30 fps', Math.abs(fps - 30) < 0.5, `${win.length} frames over ${span.toFixed(2)} s = ${fps.toFixed(2)} fps`);
  const frameGaps = video.slice(1).map((p, i) => (p.ptsUs - video[i].ptsUs) / 1000);
  rt('no dropped frames (max gap)', Math.max(...frameGaps) < 34.5, `max gap ${Math.max(...frameGaps).toFixed(1)} ms`);
  const keyNals = video.find((p) => p.keyframe)?.nal || [];
  note('NAL units in a keyframe', keyNals.join(','));

  const aGaps = audio.slice(1).map((p, i) => (p.ptsUs - audio[i].ptsUs) / 1000);
  const aSpan = (audio[audio.length - 1].ptsUs - audio[0].ptsUs) / 1e6;
  rt('audio packets continuous (21.3 ms apart)', aGaps.length > 100 && aGaps.every((g) => g > 20.5 && g < 22.2),
    `${audio.length} packets over ${aSpan.toFixed(2)} s, gaps ${Math.min(...aGaps).toFixed(2)}–${Math.max(...aGaps).toFixed(2)} ms`);
  rt('audio rate 46.875 packets/s', Math.abs((audio.length - 1) / aSpan - 46.875) < 0.3, ((audio.length - 1) / aSpan).toFixed(3));
  check('audio and video start together', Math.abs(audio[0].ptsUs - video[0].ptsUs) < 50000, `video ${video[0].ptsUs} µs, audio ${audio[0].ptsUs} µs`);

  // Bitrate before and after the live change (CBR).
  const firstPts = video[0].ptsUs;
  const rateWindow = (fromS, toS) => {
    const ps = video.filter((p) => p.ptsUs - firstPts >= fromS * 1e6 && p.ptsUs - firstPts < toS * 1e6);
    return ps.reduce((s, p) => s + p.size, 0) * 8 / (toS - fromS) / 1000;
  };
  const changePts = (rateWall - firstPacketAt) / 1000;
  const kbpsBefore = rateWindow(1, 4), kbpsAfter = rateWindow(changePts + 1.0, changePts + 5.5);
  rt('bitrate ~6000 kbps before', Math.abs(kbpsBefore - 6000) / 6000 < 0.15, `${kbpsBefore.toFixed(0)} kbps`);
  rt('live bitrate change to 2000 kbps', Math.abs(kbpsAfter - 2000) / 2000 < 0.2, `${kbpsAfter.toFixed(0)} kbps after set_bitrate`);

  // ---- FLV ------------------------------------------------------------------
  const flvFile = join(OUT_DIR, 'program.flv');
  writeFileSync(flvFile, flv.bytes());
  const probe = ffmpeg(['-i', flvFile]);
  const vLine = probe.stderr.split('\n').find((l) => /Stream #0:\d.*Video:/.test(l)) || '';
  const aLine = probe.stderr.split('\n').find((l) => /Stream #0:\d.*Audio:/.test(l)) || '';
  check(`FLV probes as h264 ${W}x${H} 30 fps`, /h264/.test(vLine) && vLine.includes(`${W}x${H}`) && /\b30 fps/.test(vLine), vLine.trim());
  check('FLV probes as aac 48000 Hz stereo', /aac/.test(aLine) && /48000 Hz/.test(aLine) && /stereo/.test(aLine), aLine.trim());
  const dec = ffmpeg(['-v', 'error', '-i', flvFile, '-f', 'null', '-']);
  check('FLV decodes without errors', dec.code === 0 && dec.stderr.trim() === '', dec.stderr.slice(0, 300));
  note('FLV size', `${(statSync(flvFile).size / 1e6).toFixed(1)} MB, ${video.length} video / ${audio.length} audio packets`);

  // ---- what Program showed: media, cut to blue, 1 s fade back to media --------
  // Found in the recording itself (10 samples a second), so the checks do not
  // depend on the wall clock; when the commands took effect is a real-time check.
  const ptsOf = (wall) => (wall - firstPacketAt) / 1000;
  const cutPts = ptsOf(cutWall), fadePts = ptsOf(fadeWall);
  const tl = blueTimeline(flvFile);
  const isBlueS = (s) => s.d < 25, isMediaS = (s) => s.d > 100;
  const iCut = tl.findIndex((s, i) => i > 0 && isBlueS(s) && isMediaS(tl[i - 1]));
  const iFade = iCut > 0 ? tl.findIndex((s, i) => i > iCut && !isBlueS(s)) : -1;
  const tCut = iCut > 0 ? tl[iCut].t : NaN, tFade = iFade > 0 ? tl[iFade].t : NaN;
  // The fade has (nearly) ended when the picture is within 10 % of the media's
  // level in the two seconds after it began; the samples before that are the mix.
  const afterFade = iFade > 0 ? tl.filter((s) => s.t >= tFade && s.t <= tFade + 2.0) : [];
  const mediaAgain = afterFade.length ? Math.max(...afterFade.map((s) => s.d)) : NaN;
  const iBack = iFade > 0 && mediaAgain > 100 ? tl.findIndex((s, i) => i >= iFade && s.d >= 0.9 * mediaAgain) : -1;
  const tBack = iBack > 0 ? tl[iBack].t : NaN;
  const mix = iFade > 0 && iBack > iFade ? tl.slice(iFade, iBack) : [];
  note('Program timeline (distance from blue, 10/s)', tl.map((s) => s.d.toFixed(0)).join(' '));
  check('set_program cut: media -> blue from one frame to the next', iCut > 0,
    iCut > 0 ? `media (${tl[iCut - 1].d.toFixed(0)}) -> blue (${tl[iCut].d.toFixed(0)}) at ${tCut.toFixed(1)} s` : 'no cut to blue found');
  rt('the cut is on air when commanded (±0.5 s)', Math.abs(tCut - cutPts) < 0.5, `commanded ${cutPts.toFixed(2)} s, seen ${tCut.toFixed(1)} s`);
  const rising = mix.every((s, i) => i === 0 || s.d >= mix[i - 1].d - 10);
  check('set_program fade: blue -> mix -> media', mix.length >= 1 && rising && iBack > 0,
    mix.length ? `blue, then ${mix.map((s) => s.d.toFixed(0)).join(' ')}, then media (${tl[iBack].d.toFixed(0)})` : `no fade found (media level after it: ${mediaAgain})`);
  rt('the fade starts when commanded and lasts ~1 s', Math.abs(tFade - fadePts) < 0.5 && tBack - tFade > 0.6 && tBack - tFade < 1.4,
    `commanded ${fadePts.toFixed(2)} s, seen ${tFade.toFixed(1)}–${tBack.toFixed(1)} s`);

  // Overlays over the media and over plain blue.
  const ovMid = [ov1.x + Math.floor(ov1.w / 2), ov1.y + Math.floor(ov1.h / 2)];
  const tOnMedia = iCut > 0 ? Math.max(0.5, tCut - 1.0) : 2.0;
  const tOnBlue = iFade > 0 ? (tCut + tl[iFade - 1].t) / 2 : NaN;
  const f2 = decodeFrame(flvFile, tOnMedia, W, H);
  const inOverlay = f2 && rgbAt(f2, W, ovMid[0], ovMid[1]);
  check(`overlay composited on Program (red layer at ${ov1.x},${ov1.y})`, inOverlay && inOverlay[0] > 200 && inOverlay[1] < 60 && inOverlay[2] < 60,
    inOverlay ? `${inOverlay.join(',')} at ${tOnMedia.toFixed(1)} s` : 'no frame');
  const afterCut = Number.isFinite(tOnBlue) ? decodeFrame(flvFile, tOnBlue, W, H) : null;
  check('a frame of plain blue (after the cut) decodes', !!afterCut && blueDistance(afterCut) < 25, Number.isFinite(tOnBlue) ? `at ${tOnBlue.toFixed(2)} s` : 'no blue segment');
  const ov5 = afterCut && rgbAt(afterCut, W, ovMid[0], ovMid[1]);
  check('overlay stays on top after the cut', ov5 && ov5[0] > 200 && ov5[2] < 60, ov5 && ov5.join(','));
  // Over plain blue: red just inside the layer's edges, blue just outside
  // (3 px margin — H.264 4:2:0 softens a sharp red/blue edge by a pixel or two).
  const x1 = ov1.x + ov1.w - 1, y1 = ov1.y + ov1.h - 1; // last pixels inside the layer
  const edge = afterCut && [[ov1.x + 3, ov1.y + 3], [x1 - 3, y1 - 3], [ov1.x - 4, ovMid[1]], [x1 + 4, ovMid[1]], [ovMid[0], ov1.y - 4], [ovMid[0], y1 + 4]]
    .map(([x, y]) => rgbAt(afterCut, W, x, y));
  const isRed = (p) => p[0] > 200 && p[1] < 60 && p[2] < 60, isBlue = (p) => p[2] > 200 && p[0] < 60 && p[1] < 60;
  check(`overlay covers exactly its rectangle (x ${ov1.x}-${x1}, y ${ov1.y}-${y1})`, edge && isRed(edge[0]) && isRed(edge[1]) && edge.slice(2).every(isBlue),
    edge && edge.map((p) => p.join('/')).join(' '));
  const semi = afterCut && rgbAt(afterCut, W, ov2.x + Math.floor(ov2.w / 2), ov2.y + Math.floor(ov2.h / 2));
  check('semi-transparent overlay blends (50 % white over blue)', semi && Math.abs(semi[0] - 128) < 30 && Math.abs(semi[1] - 128) < 30 && semi[2] > 225,
    semi && semi.join(','));

  // PCM tone (always mixed) and the clip's own 440 Hz (only while it is on Program).
  const pcm = ffmpeg(['-v', 'error', '-i', flvFile, '-ac', '1', '-ar', '48000', '-f', 'f32le', '-']);
  const samples = pcm.stdout ? new Float32Array(pcm.stdout.buffer, pcm.stdout.byteOffset, Math.floor(pcm.stdout.length / 4)) : new Float32Array(0);
  const seg = (fromS, toS) => samples.subarray(Math.floor(fromS * 48000), Math.floor(toS * 48000));
  const onAir = [1.0, Number.isFinite(tCut) ? tCut - 0.2 : 3.5], offAir = [tCut + 0.3, tFade - 0.1];
  const t1k = toneLevel(seg(...onAir), 48000, 1000), t440 = toneLevel(seg(...onAir), 48000, 440);
  const t1kBlue = toneLevel(seg(...offAir), 48000, 1000), t440Blue = toneLevel(seg(...offAir), 48000, 440);
  check('PCM tone (1 kHz) present in Program audio', t1k > 0.05 && t1kBlue > 0.05, `amplitude ${t1k.toFixed(3)} / ${t1kBlue.toFixed(3)}`);
  // A clip's sound is paced by the clock like its pictures: libobs drops what
  // arrives too late, so on a runner that cannot keep real time it thins out
  // (GitHub runners: 0.176 in real time, 0.087 lagging, nothing on the slowest).
  rt('media audio (440 Hz) on Program while the clip is on air', t440 > 0.05, `amplitude ${t440.toFixed(3)} (${onAir.map((v) => v.toFixed(1)).join('–')} s)`);
  rt('media audio leaves Program with the clip (cut to blue)', t440 > 0.05 && t440Blue < t440 / 5, `amplitude ${t440Blue.toFixed(3)} (${offAir.map((v) => v.toFixed(1)).join('–')} s)`);
  check('no clip sound once the clip is off Program', t440Blue < 0.02, `amplitude ${t440Blue.toFixed(3)}`);

  // ---- monitor feeds ----------------------------------------------------------
  // 854x480 asked for; the engine never makes a feed larger than the canvas.
  const fw = Math.min(854, W) & ~1, fh = Math.min(480, H) & ~1;
  for (const id of [0, 1]) {
    const name = id === 0 ? 'Program' : 'Preview';
    const rate = id === 0 ? 15 : 10;   // Preview at its own rate (preview_fps)
    const c = feedCfg[id];
    check(`${name} feed config ${fw}x${fh} @ ${rate} fps`, c && c.width === fw && c.height === fh && c.fpsNum / c.fpsDen === rate,
      c ? `${c.width}x${c.height} ${c.fpsNum}/${c.fpsDen} ${c.kbps} kbps` : 'none');
    const fp = feeds[id];
    const fspan = fp.length > 1 ? (fp[fp.length - 1].ptsUs - fp[0].ptsUs) / 1e6 : 0;
    check(`${name} feed starts on a keyframe`, fp.length > 0 && fp[0].keyframe, `${fp.length} packets`);
    rt(`${name} feed ~${rate} fps`, fp.length > 1 && Math.abs((fp.length - 1) / fspan - rate) < 1, `${fp.length} packets over ${fspan.toFixed(1)} s`);
    const ff = join(OUT_DIR, `feed-${id}.flv`);
    writeFileSync(ff, feedFlv[id].bytes());
    const fprobe = ffmpeg(['-i', ff]);
    const fl = fprobe.stderr.split('\n').find((l) => /Video:/.test(l)) || '';
    check(`${name} feed decodes as h264 ${fw}x${fh}`, /h264/.test(fl) && fl.includes(`${fw}x${fh}`), fl.trim());
    if (id === 1) {
      const pf = decodeFrame(ff, 1.0, fw, fh);
      const d = pf ? blueDistance(pf) : NaN;
      check('Preview feed shows the preview input (blue)', d < 25, `distance ${d.toFixed(0)}`);
    }
  }

  // ---- thumbnails -------------------------------------------------------------
  const th1 = thumbs.get(1) || [], th2 = thumbs.get(2) || [];
  check('thumbnails 256x144 RGBA for every input', th1.length >= 3 && th2.length >= 3 && [...th1, ...th2].every((t) => t.w === 256 && t.h === 144 && t.len === 256 * 144 * 4),
    `media ${th1.length}, colour ${th2.length}`);
  const px = th2[th2.length - 1]?.px || [];
  check('colour thumbnail is blue', px[2] > 200 && px[0] < 40 && px[1] < 40, px.join(','));
  const th4 = thumbs.get(4) || [];
  const ipx = th4[th4.length - 1]?.px || [];
  check('image input renders (magenta PNG thumbnail)', th4.length >= 3 && ipx[0] > 200 && ipx[1] < 40 && ipx[2] > 200, `${th4.length} thumbnails, ${ipx.join(',')}`);

  // ---- a look-up table on a source (libobs's Apply LUT; the table is the app's) ----
  // A 2x2x2 .cube that sends every colour to red; then half of it; then off.
  const lutFile = join(OUT_DIR, 'all-red.cube');
  writeFileSync(lutFile, ['LUT_3D_SIZE 2', ...Array(8).fill('1 0 0')].join('\n') + '\n');
  const lastPx = () => { const l = thumbs.get(2) || []; return l[l.length - 1]?.px || []; };
  const untilPx = async (pred) => { for (let i = 0; i < 40; i++) { if (pred(lastPx())) return true; await sleep(100); } return false; };
  await client.thumbnails({ on: true, width: 256, height: 144, fps: 10 });   // to see source 2 by itself
  const lutOn = await client.lut(2, lutFile, 1);
  const lutRed = await untilPx((p) => p[0] > 200 && p[2] < 40);
  check('a LUT on a source: the blue colour comes out red', lutOn.on === true && lutRed, lastPx().join(','));
  await client.lut(2, lutFile, 0.5);
  const halfLut = await untilPx((p) => p[0] > 90 && p[0] < 230 && p[2] > 90 && p[2] < 230 && Math.abs(p[0] - p[2]) < 40);
  check('…half of it: between the two', halfLut, lastPx().join(','));
  await client.lut(2, '');
  const lutOff = await untilPx((p) => p[2] > 200 && p[0] < 40);
  check('…and taken off: blue again', lutOff, lastPx().join(','));
  let lutMissing = '';
  try { await client.lut(2, join(OUT_DIR, 'no-such.cube')); } catch (e) { lutMissing = e.message; }
  check('a missing LUT file is an error', /not found/.test(lutMissing), lutMissing);
  // I420 thumbnails (1.5 bytes a pixel), and only when the picture changed: the blue colour is
  // sent once and then counted as unchanged; the playing clip keeps coming.
  const i420 = new Map();
  const onI420 = (t) => { if (t.format === 'i420') { const l = i420.get(t.id) || []; l.push(t); i420.set(t.id, l); } };
  client.on('thumbnail', onI420);
  const thBefore = await client.stats();
  const i420On = await client.thumbnails({ on: true, width: 256, height: 144, fps: 10, format: 'i420', only_changes: true });
  // 1.5 s at ten a second; a runner that renders slower than real time gets up to 8 s.
  for (let i = 0; i < 80 && !((i420.get(1) || []).length >= 8 && i >= 15); i++) await sleep(100);
  const thAfter = await client.stats();
  client.off('thumbnail', onI420);
  const blueI = i420.get(2) || [], clipI = i420.get(1) || [];
  const thUnchanged = thAfter.thumbnails_unchanged - thBefore.thumbnails_unchanged;
  const yuv = (t, x, y) => { const w = t.width, h = t.height, d = t.data; const Y = d[y * w + x], U = d[w * h + (y >> 1) * (w >> 1) + (x >> 1)], V = d[w * h + (w >> 1) * (h >> 1) + (y >> 1) * (w >> 1) + (x >> 1)];
    const r = Y + 1.5748 * (V - 128), g = Y - 0.1873 * (U - 128) - 0.4681 * (V - 128), b = Y + 1.8556 * (U - 128); return [r, g, b].map((v) => Math.round(Math.max(0, Math.min(255, v)))); };
  const bp = blueI[0] ? yuv(blueI[0], 128, 72) : [];
  check('I420 thumbnails: 1.5 bytes a pixel, the blue colour still blue', i420On.format === 'i420' && blueI.length >= 1 && blueI[0].data.length === 256 * 144 * 3 / 2 && bp[2] > 200 && bp[0] < 45 && bp[1] < 45,
    `${blueI.length} sent, ${blueI[0] ? blueI[0].data.length : 0} bytes, rgb ${bp.join(',')}`);
  check('only changes: the still colour is sent once, the playing clip keeps coming', blueI.length === 1 && clipI.length >= 8 && thUnchanged >= 4,
    `colour ${blueI.length}, clip ${clipI.length}, unchanged ${thUnchanged}`);
  await client.thumbnails({ on: false });
  const progLut = await client.lut('program', lutFile, 1);
  await client.lut('program', '');
  check('a LUT on Program (under the overlays) is accepted and taken off', progLut.on === true && progLut.target === 'program', JSON.stringify(progLut));

  // ---- stats ------------------------------------------------------------------
  const cpuPs = ((cpu2 - cpu1) / ((w2 - w1) / 1000)) * 100;
  note(`engine CPU (ps, ${W}x${H}p30 media + overlay + PCM + 6 Mbps program + 2 feeds + thumbnails)`, `${cpuPs.toFixed(1)} % of one core; engine's own figure ${st.cpu_percent.toFixed(1)} %`);
  note('render time', `${st.render_ms.toFixed(2)} ms average per frame`);
  note('threads while everything runs', `${threadsActive} (ps -M), engine says ${st.threads}`);
  note('memory', `${st.memory_mb} MB`);
  rt('no lagged or skipped frames in steady state (incl. cut and fade)', laggedSteady === 0 && skippedSteady === 0,
    `lagged ${laggedSteady}, skipped ${skippedSteady} (whole run: lagged ${st.frames_lagged})`);
  check('no frames dropped by the output', st.output.frames_dropped === 0, `dropped ${st.output.frames_dropped}`);
  check('PCM steered without resyncs', st.pcm[0] && st.pcm[0].resyncs === 0, st.pcm[0] ? JSON.stringify(st.pcm[0]) : 'none');
  check('stop_output drained the encoders', stopRes.was_running && stopRes.video_packets === video.length, `${stopRes.video_packets} video packets`);
  check('no pipe drops', finalStats.pipe.media_dropped === 0, JSON.stringify(finalStats.pipe));
  check('overlay updates applied', finalStats.overlay_updates === 3, finalStats.overlay_updates);
  const errors = events.filter((e) => e.ev.event === 'error');
  check('no error events', errors.length === 0, errors.map((e) => e.ev.message).join(' | '));

  // ---- projectors: Program drawn by libobs straight into a window of the engine's own ----
  // (windowed for the check — a small window, not one filling the screen; a
  // full-screen one is checked too with SKYNAT_ENGINE_TEST_FULLSCREEN=1).
  await client.setProgram(2, 'cut');   // blue
  await sleep(300);
  const rect = { x: 40, y: 80, width: 320, height: 180 };
  let proj = null, projErr = '';
  try { proj = await client.command('projector', { id: 7, on: true, content: 'program', windowed: true, display: rect }); } catch (e) { projErr = e.message; }
  check('a projector window opens and libobs gets its size in pixels', !!proj && proj.on === true && proj.width >= rect.width && proj.height >= rect.height && proj.content === 'program',
    projErr || JSON.stringify(proj));
  await sleep(900);
  const pInfo = ((await client.stats()).projectors || []).find((p) => p.id === 7);
  rt('libobs draws Program into the projector every frame', !!pInfo && pInfo.frames >= 15, JSON.stringify(pInfo));
  if (process.platform === 'darwin' && !CI && proj) {
    // What it shows, off the screen itself (only when this process may record the screen).
    const shot = join(OUT_DIR, 'projector.png');
    const cap = spawnSync('screencapture', ['-x', `-R${rect.x},${rect.y},${rect.width},${rect.height}`, shot]);
    if (cap.status === 0 && existsSync(shot)) {
      const raw = spawnSync(FFMPEG, ['-v', 'error', '-i', shot, '-vf', 'scale=32:18', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'], { maxBuffer: 1 << 20 });
      const px = raw.stdout && raw.stdout.length >= 32 * 18 * 3 ? [...raw.stdout.subarray((9 * 32 + 16) * 3, (9 * 32 + 16) * 3 + 3)] : null;
      if (px && px.some((v) => v > 0)) check('the projector shows Program on the screen (blue)', px[2] > 180 && px[0] < 60 && px[1] < 60, px.join(','));
      else note('the projector on the screen', 'not checked: this process may not record the screen (black capture)');
    }
  }
  let p2 = null;
  try { p2 = await client.command('projector', { id: 7, on: true, content: 'preview', fit: 'stretch', windowed: true, display: rect }); } catch (e) { p2 = { error: e.message }; }
  check('the same projector switches to Preview and another fit in place', !!p2 && p2.content === 'preview' && p2.fit === 'stretch' && p2.width === proj?.width, JSON.stringify(p2));
  let badErr = '';
  try { await client.command('projector', { id: 9, content: 'nope' }); } catch (e) { badErr = e.message; }
  check('a projector with an unknown content is refused', /content must be/.test(badErr), badErr);
  const closed = await client.command('projector', { id: 7, on: false });
  const afterClose = ((await client.stats()).projectors || []).some((p) => p.id === 7);
  check('a projector closes on command', closed.was_open === true && !afterClose, JSON.stringify(closed));
  if (process.env.SKYNAT_ENGINE_TEST_FULLSCREEN === '1') {
    const full = await client.command('projector', { id: 8, on: true, content: 'program' });
    check('a full-screen projector covers its display at the display\'s own pixels', full.width >= full.display.width && full.height >= full.display.height,
      `${full.width}x${full.height} px for a ${full.display.width}x${full.display.height} display (×${full.scale})`);
    await sleep(400);
    await client.command('projector', { id: 8, on: false });
  }

  // Remove a source that is on Program -> black, no crash.
  await client.setProgram(2, 'cut');
  const before = (await client.stats()).sources;
  await client.removeSource(2);
  await client.removeSource(1);
  const after = (await client.stats()).sources;
  check('sources can be removed while on air', after === before - 2, `${before} -> ${after}`);

  // Changing a running feed's size restarts it with the new size.
  const feedCfgs = [];
  const onFeedCfg = (c) => feedCfgs.push(c);
  client.on('feedConfig', onFeedCfg);
  const firstW = Math.min(640, W) & ~1;
  const feedSeen = async (w) => { for (let i = 0; i < 250 && !feedCfgs.some((c) => c.width === w); i++) await sleep(20); };
  await client.previewFeed({ on: true, program: true, preview: false, width: 640, height: 360, fps: 10 });
  await feedSeen(firstW);
  await client.previewFeed({ on: true, program: true, preview: false, width: 426, height: 240, fps: 10 });
  await feedSeen(426);
  await client.previewFeed({ on: false });
  client.off('feedConfig', onFeedCfg);
  check('monitor feed restarts with new parameters', feedCfgs.length >= 2 && feedCfgs[0].width === firstW && feedCfgs.at(-1).width === 426 && feedCfgs.at(-1).height === 240 && feedCfgs.at(-1).fpsNum / feedCfgs.at(-1).fpsDen === 10,
    feedCfgs.map((c) => `${c.width}x${c.height}@${c.fpsNum / c.fpsDen}`).join(' -> '));

  let missing = '';
  try { await client.addSource({ id: 5, type: 'media', path: join(OUT_DIR, 'no-such-file.mp4') }); } catch (e) { missing = e.message; }
  check('a missing media file is an error result', /file not found/.test(missing), missing);

  // A one-second clip that does not loop ends with a "media" event.
  // Smaller than the canvas (it is enlarged — checked below), also on a runner's small canvas.
  const shortW = Math.min(640, (W / 2) & ~1), shortH = Math.min(360, (H / 2) & ~1);
  const short = join(OUT_DIR, `clip-1s-${shortW}x${shortH}.mp4`);
  if (!existsSync(short)) ffmpeg(['-loglevel', 'error', '-y', '-f', 'lavfi', '-i', `testsrc2=size=${shortW}x${shortH}:rate=30`, '-t', '1', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', short]);
  const endedAt = Date.now();
  await client.addSource({ id: 6, type: 'media', path: short, loop: false });
  await client.setProgram(6, 'cut');
  for (let i = 0; i < 60 && !events.some((e) => e.ev.event === 'media' && e.ev.id === 6); i++) await sleep(100);
  const ended = events.find((e) => e.ev.event === 'media' && e.ev.id === 6);
  check('non-looping media reports "ended"', !!ended, ended ? `after ${ended.at - endedAt} ms` : 'no event');
  let up = null;
  for (let i = 0; i < 20 && !(up?.bicubic >= 1); i++) { up = (await client.stats()).scaled; await sleep(100); }
  check('a clip smaller than the canvas is enlarged with bicubic (not bilinear)', up?.bicubic >= 1, JSON.stringify(up));
  await client.removeSource(6);

  // An HDR clip (HLG, BT.2020): libobs turns the signal into the canvas's light; with hdr "as_is"
  // the coded signal comes through unchanged (a 75 % grey stays 191) for the app's own table.
  const hlg = join(OUT_DIR, 'hlg-grey.mp4');
  if (!existsSync(hlg)) ffmpeg(['-loglevel', 'error', '-y', '-f', 'lavfi', '-i', 'color=c=0xbfbfbf:s=640x360:r=30', '-t', '2', '-vf', 'format=yuv420p', '-c:v', 'libx264',
    '-color_range', 'tv', '-colorspace', 'bt2020nc', '-color_primaries', 'bt2020', '-color_trc', 'arib-std-b67', hlg]);
  const hlgPx = [];
  const onHlg = (t) => { if (t.id === 7 && t.format === 'rgba') hlgPx.push([...t.data.subarray((72 * t.width + 128) * 4, (72 * t.width + 128) * 4 + 3)]); };
  client.on('thumbnail', onHlg);
  await client.addSource({ id: 7, type: 'media', path: hlg, loop: true });
  await client.thumbnails({ on: true, width: 256, height: 144, fps: 10 });
  // Each picture once the change has gone through: a few thumbnails in (up to 8 s on a slow runner).
  const settled = async () => { hlgPx.length = 0; for (let i = 0; i < 80 && hlgPx.length < 4; i++) await sleep(100); return hlgPx[hlgPx.length - 1] || []; };
  await sleep(500);
  const converted = await settled();
  await client.updateSource({ id: 7, hdr: 'as_is' });
  await sleep(300);
  const asIs = await settled();
  await client.updateSource({ id: 7, hdr: 'convert' });
  await sleep(300);
  const back = await settled();
  client.off('thumbnail', onHlg);
  await client.thumbnails({ on: false });
  await client.removeSource(7);
  check('HLG clip: libobs converts it (not the coded 191)…', converted.length === 3 && Math.abs(converted[1] - 191) > 8, converted.join(','));
  check('…hdr "as_is": the coded signal as it is (191 ± 4)', asIs.length === 3 && asIs.every((v) => Math.abs(v - 191) <= 4), asIs.join(','));
  check('…and converted again when it is taken off', back.length === 3 && Math.abs(back[1] - converted[1]) <= 3, back.join(','));

  // A clip turned a quarter clockwise (a phone's portrait video): red on the left goes to the top.
  const sideways = join(OUT_DIR, 'sideways.mp4');
  if (!existsSync(sideways)) ffmpeg(['-loglevel', 'error', '-y', '-f', 'lavfi', '-i', 'color=c=red:s=160x180:r=30', '-f', 'lavfi', '-i', 'color=c=blue:s=160x180:r=30',
    '-filter_complex', '[0:v][1:v]hstack,format=yuv420p', '-t', '2', '-c:v', 'libx264', sideways]);
  let turned = null;
  const onTurn = (t) => { if (t.id === 8 && t.format === 'rgba') { const at = (x, y) => [...t.data.subarray((y * t.width + x) * 4, (y * t.width + x) * 4 + 3)]; turned = { top: at(128, 30), bottom: at(128, 114), side: at(20, 72) }; } };
  client.on('thumbnail', onTurn);
  await client.addSource({ id: 8, type: 'media', path: sideways, loop: true, rotate: 90 });
  await client.thumbnails({ on: true, width: 256, height: 144, fps: 10 });
  for (let i = 0; i < 80 && !(turned && i >= 10); i++) await sleep(100);
  client.off('thumbnail', onTurn);
  await client.thumbnails({ on: false });
  await client.removeSource(8);
  const red8 = (p) => p && p[0] > 180 && p[2] < 80, blue8 = (p) => p && p[2] > 180 && p[0] < 80, black8 = (p) => p && p[0] < 30 && p[1] < 30 && p[2] < 30;
  check('rotate 90: turned clockwise and fitted (red on top, blue below, black at the sides)', !!turned && red8(turned.top) && blue8(turned.bottom) && black8(turned.side), JSON.stringify(turned));

  // Change the canvas (outputs stopped), encode a moment at 60 fps on another size, and back
  // (1280x720; a third of the canvas on a CPU-rendered runner).
  const alt = softwareRenderer ? { w: (W / 3) & ~1, h: (H / 3) & ~1 } : { w: 1280, h: 720 };
  const cfg = await client.configure({ width: alt.w, height: alt.h, fps: 60 });
  await client.addSource({ id: 8, type: 'media', path: clip, loop: true, ...(softwareRenderer ? { hw_decode: false } : {}) });
  let down = null;
  for (let i = 0; i < 30 && !(down?.lanczos >= 1); i++) { down = (await client.stats()).scaled; await sleep(100); }
  check('…and one bigger than the canvas made smaller with Lanczos', down?.lanczos >= 1, JSON.stringify(down));
  await client.removeSource(8);
  let cfg720 = null, n720 = 0;
  const onCfg = (c) => { cfg720 = c; }, onPkt = () => { n720++; };
  client.on('videoConfig', onCfg); client.on('videoPacket', onPkt);
  await client.addSource({ id: 7, type: 'color', color: '#00ff00' });
  await client.setProgram(7);
  // The app's own tuning for an encoder, applied as given — but never B-frames
  // or keyframes off the interval (the pipe's constraints).
  const x264 = await client.startOutput({ video_kbps: 3000, encoder: 'x264', encoder_settings: { obs_x264: { preset: 'faster', tune: 'film', bf: 2, x264opts: 'aq-mode=2' }, obs_qsv11_v2: { target_usage: 'TU1' } } });
  // 1.5 s (about 90 packets in real time); a runner that cannot keep real time
  // gets up to 10 s to show that the encoder runs.
  const encodeUntil = Date.now() + (excusedRealtime ? 10000 : 1500);
  while (Date.now() < encodeUntil && !(excusedRealtime && n720 > 30)) await sleep(20);
  await client.stopOutput();
  check('software x264 only when asked for', x264.encoder === 'obs_x264' && x264.hardware === false && n720 > 30, `${x264.encoder}, ${n720} packets`);
  const xs = x264.settings || {};
  check('…opened with the app\'s settings for it, and the pipe\'s constraints kept', xs.preset === 'faster' && xs.tune === 'film' && xs.bf === 0 && /aq-mode=2/.test(xs.x264opts) && /scenecut=0/.test(xs.x264opts) && xs.bitrate === 3000,
    JSON.stringify(xs).slice(0, 200));
  client.off('videoConfig', onCfg); client.off('videoPacket', onPkt);
  const st720 = await client.stats();
  check(`configure: ${alt.w}x${alt.h} at 60 fps`, cfg.width === alt.w && cfg.fps_num === 60 && cfg720 && cfg720.width === alt.w && cfg720.height === alt.h && cfg720.fpsNum === 60 && st720.fps === 60,
    `${cfg720 ? `${cfg720.width}x${cfg720.height} ${cfg720.fpsNum}/${cfg720.fpsDen}` : 'no config'}, ${n720} packets`);
  // The stream's own smaller picture beside Program's: a second encoder, scaled, as feed 2 —
  // and its bitrate set by itself.
  let sCfg = null, sPkts = 0;
  const onSCfg = (c) => { if (c.feed === 2) sCfg = c; }, onSPkt = (p) => { if (p.feed === 2) sPkts++; };
  client.on('feedConfig', onSCfg); client.on('feedPacket', onSPkt);
  // A third of the canvas; on a runner's small canvas three quarters (the engine wants at least 64x64).
  const sk = softwareRenderer ? 0.75 : 1 / 3;
  const sw = Math.max(64, Math.round(alt.w * sk) & ~1), sh = Math.max(64, Math.round(alt.h * sk) & ~1);
  const withStream = await client.startOutput({ video_kbps: 3000, stream: { width: sw, height: sh, kbps: 800 } });
  const streamUntil = Date.now() + (excusedRealtime ? 10000 : 2000);
  while (Date.now() < streamUntil && !(sPkts > 30)) await sleep(20);
  const sRate = await client.setBitrate(600, 'stream');
  const stS = await client.stats();
  await client.stopOutput();
  client.off('feedConfig', onSCfg); client.off('feedPacket', onSPkt);
  check('start_output "stream": a smaller picture of its own as feed 2, beside Program', withStream.stream && withStream.stream.width === sw && sCfg && sCfg.width === sw && sCfg.height === sh && sPkts > 30,
    `${JSON.stringify(withStream.stream)}; feed 2 ${sCfg ? `${sCfg.width}x${sCfg.height}` : 'no config'}, ${sPkts} packets`);
  check('…its bitrate set by itself (set_bitrate target "stream")', sRate.target === 'stream' && sRate.kbps === 600 && stS.feeds && stS.feeds.stream, JSON.stringify(sRate));

  let busy = '';
  await client.startOutput({ video_kbps: 3000 });
  try { await client.configure({ width: W, height: H, fps: 30 }); } catch (e) { busy = e.message; }
  await client.stopOutput();
  check('configure refuses while the output runs', /stop the output/.test(busy), busy);
  await client.configure({ width: W, height: H, fps: 30 });

  await client.stop();
  check('shutdown command exits the engine', !client.running);

  // 3. The engine exits when its stdin closes (parent gone).
  {
    const p = spawn(ENGINE, ['--log-level', 'error'], { stdio: ['pipe', 'pipe', 'ignore'] });
    let readyAt = 0;
    p.stdout.on('data', (d) => { if (!readyAt && d.includes('"ready"')) readyAt = Date.now(); });
    for (let i = 0; i < 100 && !readyAt; i++) await sleep(50);
    const closedAt = Date.now();
    p.stdin.end();
    const code = await new Promise((r) => { const t = setTimeout(() => { p.kill('SIGKILL'); r('timeout'); }, 5000); p.on('exit', (c) => { clearTimeout(t); r(c); }); });
    check('engine exits when stdin closes', readyAt && code === 0, `exit ${code} after ${Date.now() - closedAt} ms`);
  }

  // 4. Garbage on stdin: oversized frame -> clean exit, not a hang.
  {
    const p = spawn(ENGINE, ['--log-level', 'error'], { stdio: ['pipe', 'pipe', 'ignore'] });
    let readyAt = 0;
    p.stdout.on('data', (d) => { if (!readyAt && d.includes('"ready"')) readyAt = Date.now(); });
    for (let i = 0; i < 100 && !readyAt; i++) await sleep(50);
    p.stdin.write(Buffer.from([0xff, 0xff, 0xff, 0xff, 1]));
    const code = await new Promise((r) => { const t = setTimeout(() => { p.kill('SIGKILL'); r('timeout'); }, 5000); p.on('exit', (c) => { clearTimeout(t); r(c); }); });
    check('protocol error shuts the engine down cleanly', code === 0, `exit ${code}`);
  }

  // 5. Client restarts a crashed engine with back-off.
  {
    const c = new EngineClient({ executable: ENGINE, args: ['--log-level', 'error'], backoffMs: [300] });
    const r1 = await c.start();
    let restarted = null;
    c.on('ready', (ev) => { if (ev.restarted) restarted = ev; });
    const killedAt = Date.now();
    process.kill(r1.pid, 'SIGKILL');
    for (let i = 0; i < 100 && !restarted; i++) await sleep(50);
    check('client restarts a crashed engine', restarted && restarted.pid !== r1.pid, restarted ? `new pid ${restarted.pid} after ${Date.now() - killedAt} ms` : 'no restart');
    const killed = await EngineClient.killOrphans(ENGINE);
    check('killOrphans leaves a live engine alone', killed === 0 && c.running, `killed ${killed}`);
    await c.stop();
  }

  // 6. SIGTERM -> clean exit.
  if (process.platform !== 'win32') {
    const p = spawn(ENGINE, ['--log-level', 'error'], { stdio: ['pipe', 'pipe', 'ignore'] });
    let readyAt = 0;
    p.stdout.on('data', (d) => { if (!readyAt && d.includes('"ready"')) readyAt = Date.now(); });
    for (let i = 0; i < 100 && !readyAt; i++) await sleep(50);
    p.kill('SIGTERM');
    const code = await new Promise((r) => { const t = setTimeout(() => { p.kill('SIGKILL'); r('timeout'); }, 5000); p.on('exit', (c) => { clearTimeout(t); r(c); }); });
    check('SIGTERM shuts the engine down cleanly', code === 0, `exit ${code}`);
  }

  // 7. --no-video: the pipe protocol without a renderer (what a GPU-less runner can check).
  await noVideoChecks();
}

/** Starts the engine with --no-video and checks ready, hello, refusals, stats and shutdown. */
async function noVideoChecks() {
  const c = new EngineClient({ executable: ENGINE, args: ['--no-video', '--log-level', 'warning'], restart: false, startTimeoutMs: 20000 });
  let exit = null;
  c.on('exit', (info) => { exit = info; });
  const r = await c.start();
  check('--no-video: ready without a renderer', r.event === 'ready' && r.protocol === 1 && r.video === null && r.renderer === 'none',
    `libobs ${r.libobs}, aac ${r.encoders.aac.join(', ') || 'none'}, x264 ${r.encoders.x264}`);
  const h = await c.hello();
  check('--no-video: hello answers with capabilities', h.engine === 'skynat-engine' && h.protocol === 1 && h.video === null, `version ${h.version}`);
  let refused = '';
  try { await c.addSource({ id: 1, type: 'color', color: '#ff0000' }); } catch (e) { refused = e.message; }
  check('--no-video: a video command is refused with the reason', /video is off/.test(refused), refused);
  let unknown = '';
  try { await c.command('no_such_command'); } catch (e) { unknown = e.message; }
  check('--no-video: an unknown command is still "unknown"', /unknown command/.test(unknown), unknown);
  const s = await c.stats();
  check('--no-video: stats answer', typeof s.uptime_s === 'number' && s.sources === 0 && s.output.active === false, `uptime ${s.uptime_s} s`);
  await c.stop();
  check('--no-video: shutdown command exits the engine with code 0', !c.running && exit && exit.code === 0 && exit.expected,
    exit ? `exit ${exit.code}${exit.signal ? ` (${exit.signal})` : ''}` : 'no exit seen');
}

function nalTypes(avcc) {
  const types = [];
  let off = 0;
  while (off + 4 <= avcc.length) {
    const len = avcc.readUInt32BE(off);
    if (off + 4 < avcc.length) types.push(avcc[off + 4] & 0x1f);
    off += 4 + len;
  }
  return types;
}

main().catch((e) => {
  check('test run', false, e.stack || e.message);
}).finally(() => {
  const failed = results.filter((r) => !r.ok);
  const summary = { when: new Date().toISOString(), engine: ENGINE, platform: `${process.platform}-${process.arch}`, ci: CI, mode,
    canvas: mode === 'no-gpu' ? null : `${W}x${H}`, adapter, encoder: encoderUsed, software_renderer: softwareRenderer, software_encoder: softwareEncoder,
    realtime_enforced: mode !== 'no-gpu' && !excusedRealtime, realtime_not_enforced: realtimeNotes,
    passed: results.filter((r) => r.ok && !r.note).length, failed: failed.length, results };
  writeFileSync(join(OUT_DIR, 'last-run.json'), JSON.stringify(summary, null, 2));
  writeFileSync(join(OUT_DIR, 'engine.log'), engineLog.join('\n'));
  if (failed.length && CI) {
    console.log(`\n---- the engine's log, last ${Math.min(120, engineLog.length)} of ${engineLog.length} lines ----`);
    console.log(engineLog.slice(-120).join('\n'));
    console.log('---- end of the engine log ----');
  }
  const modeText = mode === 'no-gpu' ? 'NO GPU on this runner: video checks not run, pipe checked with --no-video'
    : `full run at ${W}x${H}${softwareRenderer ? ` on a CPU rasterizer (${adapter}; no GPU on this runner)` : ''}` +
      `${softwareEncoder ? ', program on the software x264 fallback (no hardware encoder on this runner)' : encoderUsed ? `, encoder ${encoderUsed}` : ''}` +
      `${excusedRealtime ? `; NOT real time here: ${realtimeNotes.length} real-time checks reported, not enforced (${realtimeNotes.filter((r) => !r.ok).length} did not hold)` : ''}`;
  console.log(`\n${summary.passed} passed, ${failed.length} failed — ${modeText}`);
  process.exit(failed.length ? 1 : 0);
});
