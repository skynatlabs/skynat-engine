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
//                   Actions): the runners are virtual machines without a hardware video
//                   encoder, so the program may use the software x264 fallback there.
//                   Every other check stays as strict as on a real computer. If the
//                   runner's graphics cannot start at all (no Direct3D 11 device, not even
//                   WARP), the test says so plainly and checks the pipe without video
//                   (--no-video: ready, hello, refusals, stats, shutdown) instead.
//                   Local runs ignore it: a computer without hardware H.264 fails.
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
let mode = 'full';

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

  // 1. Test clip: 10 s, 1080p30, H.264 + 440 Hz AAC.
  const clip = join(OUT_DIR, 'clip-1080p30.mp4');
  if (!existsSync(clip)) {
    const r = ffmpeg(['-loglevel', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=1920x1080:rate=30', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
      '-t', '10', '-c:v', 'libx264', '-preset', 'veryfast', '-pix_fmt', 'yuv420p', '-g', '30', '-c:a', 'aac', '-b:a', '128k', '-shortest', clip]);
    check('test clip generated with ffmpeg', r.code === 0 && existsSync(clip), r.stderr.slice(0, 300));
  } else {
    check('test clip present', true, clip);
  }

  // 2. Main session.
  const log = engineLog;
  const client = new EngineClient({ executable: ENGINE, args: ['--log-level', 'info'], restart: false, startTimeoutMs: 20000, onLog: (l) => log.push(l) });
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
    mode = 'software-encoder';
    note('hardware H.264 encoder', 'none on this CI runner (a virtual machine): the software x264 fallback is accepted here (SKYNAT_ENGINE_TEST_SOFTWARE=1); every other check is unchanged');
    check('software x264 fallback available', ready.encoders.x264 === true);
  }
  check('AAC encoder found', ready.encoders.aac.length > 0, ready.encoders.aac.join(', '));
  const pid = ready.pid;
  note('threads at idle', `${threadCount(pid)} (ps -M)`);

  const hello = await client.hello();
  check('hello answers with capabilities', hello.engine === 'skynat-engine' && hello.protocol === 1, `version ${hello.version}`);
  if (CI) await client.stats(1000);
  const devices = await client.listDevices();
  note('cameras', `${devices.cameras.length} found, permission ${devices.camera_permission}: ${devices.cameras.map((c) => `${c.name}${c.best_format ? ` (${c.best_format.width}x${c.best_format.height}@${c.best_format.fps})` : ''}`).join(' | ')}`);

  let unknownOk = false;
  try { await client.command('no_such_command'); } catch (e) { unknownOk = /unknown command/.test(e.message); }
  check('unknown command is answered with an error result', unknownOk);

  await client.addSource({ id: 1, type: 'media', path: clip, loop: true });
  await client.addSource({ id: 2, type: 'color', color: '#0000ff' });
  await client.addSource({ id: 10, type: 'overlay', width: 400, height: 200, x: 100, y: 100 });
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
  const red = Buffer.alloc(400 * 200 * 4);
  for (let i = 0; i < red.length; i += 4) { red[i] = 255; red[i + 1] = 0; red[i + 2] = 0; red[i + 3] = 255; }
  client.sendOverlay(10, 0, 0, 400, 100, 1600, red, true);
  client.sendOverlay(10, 0, 100, 400, 100, 1600, red.subarray(400 * 100 * 4));
  // A second layer: 50 % white, straight alpha, at (1200, 600) 200x100.
  await client.addSource({ id: 11, type: 'overlay', width: 200, height: 100, x: 1200, y: 600 });
  const half = Buffer.alloc(200 * 100 * 4);
  for (let i = 0; i < half.length; i += 4) { half[i] = 255; half[i + 1] = 255; half[i + 2] = 255; half[i + 3] = 128; }
  client.sendOverlay(11, 0, 0, 200, 100, 800, half);

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
  if (out.hardware === true || !SOFTWARE_OK) {
    check('output started on the hardware encoder', out.hardware === true, `${out.encoder} + ${out.audio_encoder}`);
  } else {
    mode = 'software-encoder';
    check('output started on the software x264 fallback (CI runner without a hardware encoder)', out.encoder === 'obs_x264',
      `${out.encoder} + ${out.audio_encoder}`);
  }
  const tOut = Date.now();
  const at = (s) => sleep(Math.max(0, tOut + s * 1000 - Date.now()));

  await client.previewFeed({ on: true, width: 854, height: 480, fps: 15, kbps: 800 });
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

  // ---- program stream -------------------------------------------------------
  check('avcC received before video', vcfg && vcfg.avcC[0] === 1 && vcfg.width === 1920 && vcfg.height === 1080,
    vcfg ? `profile 0x${vcfg.avcC[1].toString(16)} level ${vcfg.avcC[3] / 10}, ${vcfg.fpsNum}/${vcfg.fpsDen} fps, ${vcfg.kbps} kbps` : 'none');
  check('H.264 High profile', vcfg && vcfg.avcC[1] === 100);
  check('AudioSpecificConfig received (AAC-LC 48 kHz stereo)', acfg && acfg.sampleRate === 48000 && acfg.channels === 2 && ((acfg.asc[0] >> 3) === 2),
    acfg ? acfg.asc.toString('hex') : 'none');
  check('first video packet is a keyframe', video.length > 0 && video[0].keyframe);

  const win = video.filter((p) => p.ptsUs <= 10e6);
  const keys = video.filter((p) => p.keyframe).map((p) => p.ptsUs / 1e6);
  const keyGaps = keys.slice(1).map((k, i) => k - keys[i]);
  check('keyframes every ~2 s', keyGaps.length >= 3 && keyGaps.every((g) => Math.abs(g - 2) < 0.07),
    keys.map((k) => k.toFixed(2)).join(', '));
  let mono = true, dtsEq = true;
  for (let i = 1; i < video.length; i++) { if (video[i].ptsUs <= video[i - 1].ptsUs) mono = false; }
  for (const p of video) if (p.dtsUs !== p.ptsUs) dtsEq = false;
  check('video timestamps strictly increasing', mono);
  check('no B-frames (dts == pts)', dtsEq);
  const span = (win[win.length - 1].ptsUs - win[0].ptsUs) / 1e6;
  const fps = (win.length - 1) / span;
  check('~30 fps', Math.abs(fps - 30) < 0.5, `${win.length} frames over ${span.toFixed(2)} s = ${fps.toFixed(2)} fps`);
  const frameGaps = video.slice(1).map((p, i) => (p.ptsUs - video[i].ptsUs) / 1000);
  check('no dropped frames (max gap)', Math.max(...frameGaps) < 34.5, `max gap ${Math.max(...frameGaps).toFixed(1)} ms`);
  const keyNals = video.find((p) => p.keyframe)?.nal || [];
  note('NAL units in a keyframe', keyNals.join(','));

  const aGaps = audio.slice(1).map((p, i) => (p.ptsUs - audio[i].ptsUs) / 1000);
  const aSpan = (audio[audio.length - 1].ptsUs - audio[0].ptsUs) / 1e6;
  check('audio packets continuous (21.3 ms apart)', aGaps.length > 100 && aGaps.every((g) => g > 20.5 && g < 22.2),
    `${audio.length} packets over ${aSpan.toFixed(2)} s, gaps ${Math.min(...aGaps).toFixed(2)}–${Math.max(...aGaps).toFixed(2)} ms`);
  check('audio rate 46.875 packets/s', Math.abs((audio.length - 1) / aSpan - 46.875) < 0.3, ((audio.length - 1) / aSpan).toFixed(3));
  check('audio and video start together', Math.abs(audio[0].ptsUs - video[0].ptsUs) < 50000, `video ${video[0].ptsUs} µs, audio ${audio[0].ptsUs} µs`);

  // Bitrate before and after the live change (CBR).
  const firstPts = video[0].ptsUs;
  const rateWindow = (fromS, toS) => {
    const ps = video.filter((p) => p.ptsUs - firstPts >= fromS * 1e6 && p.ptsUs - firstPts < toS * 1e6);
    return ps.reduce((s, p) => s + p.size, 0) * 8 / (toS - fromS) / 1000;
  };
  const changePts = (rateWall - firstPacketAt) / 1000;
  const kbpsBefore = rateWindow(1, 4), kbpsAfter = rateWindow(changePts + 1.0, changePts + 5.5);
  check('bitrate ~6000 kbps before', Math.abs(kbpsBefore - 6000) / 6000 < 0.15, `${kbpsBefore.toFixed(0)} kbps`);
  check('live bitrate change to 2000 kbps', Math.abs(kbpsAfter - 2000) / 2000 < 0.2, `${kbpsAfter.toFixed(0)} kbps after set_bitrate`);

  // ---- FLV ------------------------------------------------------------------
  const flvFile = join(OUT_DIR, 'program.flv');
  writeFileSync(flvFile, flv.bytes());
  const probe = ffmpeg(['-i', flvFile]);
  const vLine = probe.stderr.split('\n').find((l) => /Stream #0:\d.*Video:/.test(l)) || '';
  const aLine = probe.stderr.split('\n').find((l) => /Stream #0:\d.*Audio:/.test(l)) || '';
  check('FLV probes as h264 1920x1080 30 fps', /h264/.test(vLine) && /1920x1080/.test(vLine) && /\b30 fps/.test(vLine), vLine.trim());
  check('FLV probes as aac 48000 Hz stereo', /aac/.test(aLine) && /48000 Hz/.test(aLine) && /stereo/.test(aLine), aLine.trim());
  const dec = ffmpeg(['-v', 'error', '-i', flvFile, '-f', 'null', '-']);
  check('FLV decodes without errors', dec.code === 0 && dec.stderr.trim() === '', dec.stderr.slice(0, 300));
  note('FLV size', `${(statSync(flvFile).size / 1e6).toFixed(1)} MB, ${video.length} video / ${audio.length} audio packets`);

  // Overlay over media (t=2 s) and over blue (t=5 s); cut and fade.
  const ptsOf = (wall) => (wall - firstPacketAt) / 1000;
  const f2 = decodeFrame(flvFile, 2.0, 1920, 1080);
  const inOverlay = f2 && rgbAt(f2, 1920, 300, 200);
  check('overlay composited on Program (red layer at 100,100)', inOverlay && inOverlay[0] > 200 && inOverlay[1] < 60 && inOverlay[2] < 60, inOverlay && inOverlay.join(','));
  const cutPts = ptsOf(cutWall);
  const beforeCut = decodeFrame(flvFile, cutPts - 0.5, 1920, 1080), afterCut = decodeFrame(flvFile, cutPts + 0.5, 1920, 1080);
  const dBefore = beforeCut ? blueDistance(beforeCut) : NaN, dAfter = afterCut ? blueDistance(afterCut) : NaN;
  check('set_program cut: media -> blue', dBefore > 100 && dAfter < 25, `distance from blue ${dBefore.toFixed(0)} -> ${dAfter.toFixed(0)}`);
  const ov5 = afterCut && rgbAt(afterCut, 1920, 300, 200);
  check('overlay stays on top after the cut', ov5 && ov5[0] > 200 && ov5[2] < 60, ov5 && ov5.join(','));
  // Over plain blue: red just inside the layer's edges, blue just outside
  // (3 px margin — H.264 4:2:0 softens a sharp red/blue edge by a pixel or two).
  const edge = afterCut && [[103, 103], [496, 296], [96, 200], [503, 200], [300, 96], [300, 303]].map(([x, y]) => rgbAt(afterCut, 1920, x, y));
  const isRed = (p) => p[0] > 200 && p[1] < 60 && p[2] < 60, isBlue = (p) => p[2] > 200 && p[0] < 60 && p[1] < 60;
  check('overlay covers exactly its rectangle (x 100-499, y 100-299)', edge && isRed(edge[0]) && isRed(edge[1]) && edge.slice(2).every(isBlue),
    edge && edge.map((p) => p.join('/')).join(' '));
  const semi = afterCut && rgbAt(afterCut, 1920, 1300, 650);
  check('semi-transparent overlay blends (50 % white over blue)', semi && Math.abs(semi[0] - 128) < 30 && Math.abs(semi[1] - 128) < 30 && semi[2] > 225,
    semi && semi.join(','));
  const fadePts = ptsOf(fadeWall);
  const fd = [-0.25, 0.5, 1.6].map((dt) => { const f = decodeFrame(flvFile, fadePts + dt, 1920, 1080); return f ? blueDistance(f) : NaN; });
  check('set_program fade (1 s): blue -> mix -> media', fd[0] < 25 && fd[1] > fd[0] + 15 && fd[1] < fd[2] - 15 && fd[2] > 100,
    fd.map((d) => d.toFixed(0)).join(' -> '));

  // PCM tone (always mixed) and the clip's own 440 Hz (only while it is on Program).
  const pcm = ffmpeg(['-v', 'error', '-i', flvFile, '-ac', '1', '-ar', '48000', '-f', 'f32le', '-']);
  const samples = pcm.stdout ? new Float32Array(pcm.stdout.buffer, pcm.stdout.byteOffset, Math.floor(pcm.stdout.length / 4)) : new Float32Array(0);
  const seg = (fromS, toS) => samples.subarray(Math.floor(fromS * 48000), Math.floor(toS * 48000));
  const t1k = goertzel(seg(1, 3.5), 48000, 1000), t440 = goertzel(seg(1, 3.5), 48000, 440);
  const t1kBlue = goertzel(seg(cutPts + 0.3, fadePts - 0.1), 48000, 1000), t440Blue = goertzel(seg(cutPts + 0.3, fadePts - 0.1), 48000, 440);
  check('PCM tone (1 kHz) present in Program audio', t1k > 0.05 && t1kBlue > 0.05, `amplitude ${t1k.toFixed(3)} / ${t1kBlue.toFixed(3)}`);
  check('media audio (440 Hz) on Program while the clip is on air', t440 > 0.05, `amplitude ${t440.toFixed(3)}`);
  check('media audio leaves Program with the clip (cut to blue)', t440Blue < t440 / 5, `amplitude ${t440Blue.toFixed(3)}`);

  // ---- monitor feeds ----------------------------------------------------------
  for (const id of [0, 1]) {
    const name = id === 0 ? 'Program' : 'Preview';
    const c = feedCfg[id];
    check(`${name} feed config 854x480 @ 15 fps`, c && c.width === 854 && c.height === 480 && c.fpsNum / c.fpsDen === 15,
      c ? `${c.width}x${c.height} ${c.fpsNum}/${c.fpsDen} ${c.kbps} kbps` : 'none');
    const fp = feeds[id];
    const fspan = fp.length > 1 ? (fp[fp.length - 1].ptsUs - fp[0].ptsUs) / 1e6 : 0;
    check(`${name} feed ~15 fps, starts on a keyframe`, fp.length > 0 && fp[0].keyframe && Math.abs((fp.length - 1) / fspan - 15) < 1,
      `${fp.length} packets over ${fspan.toFixed(1)} s`);
    const ff = join(OUT_DIR, `feed-${id}.flv`);
    writeFileSync(ff, feedFlv[id].bytes());
    const fprobe = ffmpeg(['-i', ff]);
    const fl = fprobe.stderr.split('\n').find((l) => /Video:/.test(l)) || '';
    check(`${name} feed decodes as h264 854x480`, /h264/.test(fl) && /854x480/.test(fl), fl.trim());
    if (id === 1) {
      const pf = decodeFrame(ff, 1.0, 854, 480);
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

  // ---- stats ------------------------------------------------------------------
  const cpuPs = ((cpu2 - cpu1) / ((w2 - w1) / 1000)) * 100;
  note('engine CPU (ps, 1080p30 media + overlay + PCM + 6 Mbps program + 2 feeds + thumbnails)', `${cpuPs.toFixed(1)} % of one core; engine's own figure ${st.cpu_percent.toFixed(1)} %`);
  note('render time', `${st.render_ms.toFixed(2)} ms average per frame`);
  note('threads while everything runs', `${threadsActive} (ps -M), engine says ${st.threads}`);
  note('memory', `${st.memory_mb} MB`);
  const lagged = st.frames_lagged - st1.frames_lagged, skipped = st.frames_skipped - st1.frames_skipped;
  check('no lagged or skipped frames in steady state (incl. cut and fade)', lagged === 0 && skipped === 0 && st.output.frames_dropped === 0,
    `lagged ${lagged}, skipped ${skipped}, dropped ${st.output.frames_dropped} (whole run: lagged ${st.frames_lagged})`);
  check('PCM steered without resyncs', st.pcm[0] && st.pcm[0].resyncs === 0, st.pcm[0] ? JSON.stringify(st.pcm[0]) : 'none');
  check('stop_output drained the encoders', stopRes.was_running && stopRes.video_packets === video.length, `${stopRes.video_packets} video packets`);
  check('no pipe drops', finalStats.pipe.media_dropped === 0, JSON.stringify(finalStats.pipe));
  check('overlay updates applied', finalStats.overlay_updates === 3, finalStats.overlay_updates);
  const errors = events.filter((e) => e.ev.event === 'error');
  check('no error events', errors.length === 0, errors.map((e) => e.ev.message).join(' | '));

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
  await client.previewFeed({ on: true, program: true, preview: false, width: 640, height: 360, fps: 10 });
  await sleep(700);
  await client.previewFeed({ on: true, program: true, preview: false, width: 426, height: 240, fps: 10 });
  await sleep(700);
  await client.previewFeed({ on: false });
  client.off('feedConfig', onFeedCfg);
  check('monitor feed restarts with new parameters', feedCfgs.length >= 2 && feedCfgs[0].width === 640 && feedCfgs.at(-1).width === 426 && feedCfgs.at(-1).height === 240 && feedCfgs.at(-1).fpsNum / feedCfgs.at(-1).fpsDen === 10,
    feedCfgs.map((c) => `${c.width}x${c.height}@${c.fpsNum / c.fpsDen}`).join(' -> '));

  let missing = '';
  try { await client.addSource({ id: 5, type: 'media', path: join(OUT_DIR, 'no-such-file.mp4') }); } catch (e) { missing = e.message; }
  check('a missing media file is an error result', /file not found/.test(missing), missing);

  // A one-second clip that does not loop ends with a "media" event.
  const short = join(OUT_DIR, 'clip-1s.mp4');
  if (!existsSync(short)) ffmpeg(['-loglevel', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=640x360:rate=30', '-t', '1', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', short]);
  const endedAt = Date.now();
  await client.addSource({ id: 6, type: 'media', path: short, loop: false });
  await client.setProgram(6, 'cut');
  for (let i = 0; i < 60 && !events.some((e) => e.ev.event === 'media' && e.ev.id === 6); i++) await sleep(100);
  const ended = events.find((e) => e.ev.event === 'media' && e.ev.id === 6);
  check('non-looping media reports "ended"', !!ended, ended ? `after ${ended.at - endedAt} ms` : 'no event');
  await client.removeSource(6);

  // Change the canvas (outputs stopped), encode a moment at 1280x720 60 fps, and back.
  const cfg = await client.configure({ width: 1280, height: 720, fps: 60 });
  let cfg720 = null, n720 = 0;
  const onCfg = (c) => { cfg720 = c; }, onPkt = () => { n720++; };
  client.on('videoConfig', onCfg); client.on('videoPacket', onPkt);
  await client.addSource({ id: 7, type: 'color', color: '#00ff00' });
  await client.setProgram(7);
  const x264 = await client.startOutput({ video_kbps: 3000, encoder: 'x264' });
  await sleep(1500);
  await client.stopOutput();
  check('software x264 only when asked for', x264.encoder === 'obs_x264' && x264.hardware === false && n720 > 30, `${x264.encoder}, ${n720} packets`);
  client.off('videoConfig', onCfg); client.off('videoPacket', onPkt);
  const st720 = await client.stats();
  check('configure: 1280x720 at 60 fps', cfg.width === 1280 && cfg.fps_num === 60 && cfg720 && cfg720.width === 1280 && cfg720.height === 720 && cfg720.fpsNum === 60 && st720.fps === 60,
    `${cfg720 ? `${cfg720.width}x${cfg720.height} ${cfg720.fpsNum}/${cfg720.fpsDen}` : 'no config'}, ${n720} packets in 1.5 s`);
  let busy = '';
  await client.startOutput({ video_kbps: 3000 });
  try { await client.configure({ width: 1920, height: 1080, fps: 30 }); } catch (e) { busy = e.message; }
  await client.stopOutput();
  check('configure refuses while the output runs', /stop the output/.test(busy), busy);
  await client.configure({ width: 1920, height: 1080, fps: 30 });

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
    passed: results.filter((r) => r.ok && !r.note).length, failed: failed.length, results };
  writeFileSync(join(OUT_DIR, 'last-run.json'), JSON.stringify(summary, null, 2));
  writeFileSync(join(OUT_DIR, 'engine.log'), engineLog.join('\n'));
  if (failed.length && CI) {
    console.log(`\n---- the engine's log, last ${Math.min(120, engineLog.length)} of ${engineLog.length} lines ----`);
    console.log(engineLog.slice(-120).join('\n'));
    console.log('---- end of the engine log ----');
  }
  const modeText = { full: 'full run', 'software-encoder': 'full run on the software x264 fallback (CI runner without a hardware encoder)',
    'no-gpu': 'NO GPU on this runner: video checks not run, pipe checked with --no-video' }[mode];
  console.log(`\n${summary.passed} passed, ${failed.length} failed — ${modeText}`);
  process.exit(failed.length ? 1 : 0);
});
