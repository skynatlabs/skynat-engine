// Unit tests for the protocol framing in client/engine-client.ts (no engine needed).
//   node tests/framing.test.mjs        (Node 22.18+ runs the .ts client with type stripping)
//
// SPDX-License-Identifier: GPL-2.0-or-later
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  FrameParser, encodeFrame, encodeCommand, encodeOverlay, encodePcm, MsgType, MAX_PAYLOAD,
  parseVideoConfig, parseVideoPacket, parseAudioConfig, parseAudioPacket, parseFeedConfig, parseFeedPacket, parseThumbnail,
} from '../client/engine-client.ts';

function collect(chunks) {
  const out = [];
  const p = new FrameParser((type, payload) => out.push({ type, payload: Buffer.from(payload) }));
  for (const c of chunks) p.push(c);
  return out;
}

function randomFrames(n, seed = 1) {
  let s = seed;
  const rnd = () => ((s = (s * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
  const frames = [];
  for (let i = 0; i < n; i++) {
    const len = i % 7 === 0 ? 0 : Math.floor(rnd() * 5000);
    const payload = Buffer.alloc(len);
    for (let k = 0; k < len; k++) payload[k] = Math.floor(rnd() * 256);
    frames.push({ type: 1 + (i % 11), payload });
  }
  return frames;
}

test('header layout: u32 LE length + type byte', () => {
  const f = encodeFrame(4, Buffer.from([1, 2, 3]));
  assert.deepEqual([...f], [3, 0, 0, 0, 4, 1, 2, 3]);
  assert.deepEqual([...encodeFrame(2)], [0, 0, 0, 0, 2]);
});

test('round trip: one buffer, byte-by-byte and random chunking', () => {
  const frames = randomFrames(200);
  const stream = Buffer.concat(frames.map((f) => encodeFrame(f.type, f.payload)));
  const check = (got) => {
    assert.equal(got.length, frames.length);
    got.forEach((g, i) => { assert.equal(g.type, frames[i].type); assert.ok(g.payload.equals(frames[i].payload), `frame ${i}`); });
  };
  check(collect([stream]));
  const bytes = [];
  for (let i = 0; i < Math.min(stream.length, 20000); i++) bytes.push(stream.subarray(i, i + 1));
  const prefixFrames = collect(bytes);
  assert.ok(prefixFrames.length > 0);
  prefixFrames.forEach((g, i) => assert.ok(g.payload.equals(frames[i].payload)));
  for (let seed = 1; seed <= 20; seed++) {
    let s = seed; const chunks = []; let off = 0;
    while (off < stream.length) { s = (s * 69069 + 1) >>> 0; const n = 1 + (s % 9000); chunks.push(stream.subarray(off, off + n)); off += n; }
    check(collect(chunks));
  }
});

test('frames split across the 5-byte header', () => {
  const a = encodeFrame(2, Buffer.from('{"event":"ready"}'));
  const b = encodeFrame(4, Buffer.alloc(1000, 7));
  const s = Buffer.concat([a, b]);
  const got = collect([s.subarray(0, 2), s.subarray(2, a.length + 3), s.subarray(a.length + 3)]);
  assert.equal(got.length, 2);
  assert.equal(got[0].payload.toString(), '{"event":"ready"}');
  assert.equal(got[1].payload.length, 1000);
});

test('oversized frame is a protocol error', () => {
  const p = new FrameParser(() => {});
  const h = Buffer.alloc(5); h.writeUInt32LE(MAX_PAYLOAD + 1, 0); h[4] = 1;
  assert.throws(() => p.push(h), /protocol error/);
});

test('command is JSON with the cmd field', () => {
  const f = encodeCommand({ cmd: 'hello', req: 3 });
  assert.equal(f[4], MsgType.COMMAND);
  assert.deepEqual(JSON.parse(f.subarray(5).toString()), { cmd: 'hello', req: 3 });
});

test('overlay message layout', () => {
  const px = new Uint8Array(2 * 3 * 4).map((_, i) => i);
  const f = encodeOverlay(9, 10, 20, 2, 3, 8, px, 1);
  assert.equal(f[4], MsgType.OVERLAY);
  assert.equal(f.readUInt32LE(0), 28 + 24);
  const p = f.subarray(5);
  assert.deepEqual([0, 4, 8, 12, 16, 20, 24].map((o) => p.readUInt32LE(o)), [9, 10, 20, 2, 3, 8, 1]);
  assert.deepEqual([...p.subarray(28)], [...px]);
  assert.throws(() => encodeOverlay(1, 0, 0, 4, 4, 16, new Uint8Array(10)), /need/);
});

test('pcm message layout', () => {
  const s = new Float32Array([0.5, -0.5, 0.25, -0.25]);
  const f = encodePcm(3, s, 2500);
  assert.equal(f[4], MsgType.PCM);
  const p = f.subarray(5);
  assert.equal(p.readUInt32LE(0), 3);
  assert.equal(p.readUInt32LE(4), 2);
  assert.equal(p.readUInt32LE(8), 2500);
  assert.equal(p.length, 16 + 16);
  assert.equal(p.readFloatLE(16), 0.5);
  assert.equal(p.readFloatLE(28), -0.25);
});

test('engine -> app payload parsers', () => {
  const vc = Buffer.alloc(16 + 3); vc.writeUInt16LE(1920, 0); vc.writeUInt16LE(1080, 2); vc.writeUInt32LE(30, 4); vc.writeUInt32LE(1, 8); vc.writeUInt32LE(6000, 12); vc.set([1, 0x64, 0], 16);
  assert.deepEqual({ ...parseVideoConfig(vc), avcC: [...parseVideoConfig(vc).avcC] }, { width: 1920, height: 1080, fpsNum: 30, fpsDen: 1, kbps: 6000, avcC: [1, 0x64, 0] });
  const vp = Buffer.alloc(17 + 2); vp.writeBigInt64LE(2000000n, 0); vp.writeBigInt64LE(1966667n, 8); vp[16] = 1; vp.set([9, 9], 17);
  const pv = parseVideoPacket(vp);
  assert.equal(pv.ptsUs, 2000000); assert.equal(pv.dtsUs, 1966667); assert.equal(pv.keyframe, true); assert.deepEqual([...pv.data], [9, 9]);
  const ac = Buffer.alloc(14); ac.writeUInt32LE(48000, 0); ac.writeUInt16LE(2, 4); ac.writeUInt32LE(160, 8); ac.set([0x11, 0x90], 12);
  assert.deepEqual({ ...parseAudioConfig(ac), asc: [...parseAudioConfig(ac).asc] }, { sampleRate: 48000, channels: 2, kbps: 160, asc: [0x11, 0x90] });
  const ap = Buffer.alloc(9); ap.writeBigInt64LE(-5n, 0); ap[8] = 0xaa;
  assert.equal(parseAudioPacket(ap).ptsUs, -5);
  const fc = Buffer.concat([Buffer.from([1]), vc]);
  assert.equal(parseFeedConfig(fc).feed, 1); assert.equal(parseFeedConfig(fc).width, 1920);
  const fp = Buffer.concat([Buffer.from([0]), vp]);
  assert.equal(parseFeedPacket(fp).feed, 0); assert.equal(parseFeedPacket(fp).keyframe, true);
  const th = Buffer.alloc(16 + 8); th.writeUInt32LE(5, 0); th.writeUInt16LE(1, 4); th.writeUInt16LE(2, 6); th.writeUInt32LE(4, 12);
  const t = parseThumbnail(th);
  assert.deepEqual([t.id, t.width, t.height, t.stride, t.data.length, t.format], [5, 1, 2, 4, 8, 'rgba']);
});
