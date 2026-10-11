// Minimal FLV writer for the tests — the same tag layout as Skynat Live's
// electron/flv-mux.ts (AVC sequence header from the avcC, AAC sequence header
// from the AudioSpecificConfig, CompositionTime 0, timestamps in ms), so a
// file that probes cleanly here muxes cleanly in the app.
//
// SPDX-License-Identifier: GPL-2.0-or-later

const TAG_AUDIO = 8;
const TAG_VIDEO = 9;
const TAG_SCRIPT = 18;

function amfString(s) {
  const b = Buffer.from(s, 'utf8');
  const h = Buffer.alloc(3);
  h[0] = 0x02;
  h.writeUInt16BE(b.length, 1);
  return Buffer.concat([h, b]);
}

function onMetaData(v, a) {
  const entries = [['duration', 0]];
  if (v) entries.push(['width', v.width], ['height', v.height], ['videodatarate', v.kbps], ['framerate', v.fpsNum / v.fpsDen], ['videocodecid', 7]);
  if (a) entries.push(['audiodatarate', a.kbps], ['audiosamplerate', a.sampleRate], ['audiosamplesize', 16], ['stereo', a.channels > 1], ['audiocodecid', 10]);
  entries.push(['encoder', 'skynat-engine test']);
  const parts = [amfString('onMetaData')];
  const head = Buffer.alloc(5);
  head[0] = 0x08;
  head.writeUInt32BE(entries.length, 1);
  parts.push(head);
  for (const [k, val] of entries) {
    const key = Buffer.from(k, 'utf8');
    const kl = Buffer.alloc(2);
    kl.writeUInt16BE(key.length);
    parts.push(kl, key);
    if (typeof val === 'number') { const n = Buffer.alloc(9); n[0] = 0; n.writeDoubleBE(val, 1); parts.push(n); }
    else if (typeof val === 'boolean') parts.push(Buffer.from([0x01, val ? 1 : 0]));
    else parts.push(amfString(val));
  }
  parts.push(Buffer.from([0, 0, 9]));
  return Buffer.concat(parts);
}

export class FlvWriter {
  constructor({ video = true, audio = true } = {}) {
    this.wantVideo = video;
    this.wantAudio = audio;
    this.video = null;
    this.audio = null;
    this.started = false;
    this.parts = [];
    this.pending = [];
    this.lastTs = { [TAG_VIDEO]: -1, [TAG_AUDIO]: -1 };
    this.sawKey = false;
  }

  tag(type, ts, bodies) {
    let size = 0;
    for (const b of bodies) size += b.length;
    const t = Buffer.alloc(11 + size + 4);
    t[0] = type;
    t.writeUIntBE(size, 1, 3);
    t.writeUIntBE(ts & 0xffffff, 4, 3);
    t[7] = (ts >>> 24) & 0xff;
    let off = 11;
    for (const b of bodies) { b.copy(t, off); off += b.length; }
    t.writeUInt32BE(11 + size, off);
    this.parts.push(t);
  }

  tryStart() {
    if (this.started) return;
    if ((this.wantVideo && !this.video) || (this.wantAudio && !this.audio)) return;
    this.started = true;
    const flags = (this.wantAudio ? 0x04 : 0) | (this.wantVideo ? 0x01 : 0);
    this.parts.push(Buffer.from([0x46, 0x4c, 0x56, 0x01, flags, 0, 0, 0, 9, 0, 0, 0, 0]));
    this.tag(TAG_SCRIPT, 0, [onMetaData(this.video, this.audio)]);
    if (this.video) this.tag(TAG_VIDEO, 0, [Buffer.from([0x17, 0x00, 0, 0, 0]), this.video.avcC]);
    if (this.audio) this.tag(TAG_AUDIO, 0, [Buffer.from([0xaf, 0x00]), this.audio.asc]);
    for (const p of this.pending) this.write(p);
    this.pending = [];
  }

  videoConfig(cfg) {
    const changed = this.video && !this.video.avcC.equals(cfg.avcC);
    this.video = cfg;
    if (this.started && changed) this.tag(TAG_VIDEO, Math.max(0, this.lastTs[TAG_VIDEO]), [Buffer.from([0x17, 0x00, 0, 0, 0]), cfg.avcC]);
    this.tryStart();
  }

  audioConfig(cfg) {
    this.audio = cfg;
    this.tryStart();
  }

  write(p) {
    if (!this.started) { this.pending.push(p); return; }
    const ts = Math.max(this.lastTs[p.type], Math.max(0, Math.round(p.ptsUs / 1000)));
    this.lastTs[p.type] = ts;
    this.tag(p.type, ts, p.bodies);
  }

  videoPacket(pkt) {
    if (!this.sawKey) { if (!pkt.keyframe) return; this.sawKey = true; }
    this.write({ type: TAG_VIDEO, ptsUs: pkt.ptsUs, bodies: [Buffer.from([pkt.keyframe ? 0x17 : 0x27, 0x01, 0, 0, 0]), pkt.data] });
  }

  audioPacket(pkt) {
    this.write({ type: TAG_AUDIO, ptsUs: pkt.ptsUs, bodies: [Buffer.from([0xaf, 0x01]), pkt.data] });
  }

  bytes() {
    return Buffer.concat(this.parts);
  }
}
