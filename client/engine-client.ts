// engine-client.ts — the app side of the skynat-engine pipe protocol.
//
// Dependency-free (node:child_process + Buffer only). Starts the engine as a
// hidden child process, frames and parses messages (PROTOCOL.md), offers
// typed commands and events, restarts the engine with back-off if it
// crashes, and cleans up orphaned engines left by a crashed app.
//
// This file talks to the engine only through its stdin/stdout. It contains
// no OBS code and does not link or load libobs, so it is MIT-licensed and
// may be copied into closed-source apps such as Skynat Live. The engine
// itself is GPL-2.0-or-later (https://github.com/skynatlabs/skynat-engine).
//
// Copyright (C) 2026 Skynat
// SPDX-License-Identifier: MIT
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions: The above copyright notice and this permission
// notice shall be included in all copies or substantial portions of the
// Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// Written in "erasable" TypeScript (no enums, namespaces or parameter
// properties) so Node can also run it directly with type stripping.

import { execFile, spawn } from 'node:child_process';
import type { ChildProcess, SpawnOptions } from 'node:child_process';

// ---------------------------------------------------------------------------
// Protocol constants
// ---------------------------------------------------------------------------

export const PROTOCOL_VERSION = 1;
export const HEADER_SIZE = 5;
export const MAX_PAYLOAD = 64 * 1024 * 1024;

export const MsgType = {
  COMMAND: 1,
  EVENT: 2,
  VIDEO_CONFIG: 3,
  VIDEO_PACKET: 4,
  AUDIO_CONFIG: 5,
  AUDIO_PACKET: 6,
  OVERLAY: 7,
  PCM: 8,
  FEED_CONFIG: 9,
  FEED_PACKET: 10,
  THUMBNAIL: 11,
} as const;

export const Feed = { PROGRAM: 0, PREVIEW: 1 } as const;
export type FeedId = 0 | 1;

export const OVERLAY_CLEAR = 1;

// ---------------------------------------------------------------------------
// Message shapes
// ---------------------------------------------------------------------------

export interface VideoConfig { width: number; height: number; fpsNum: number; fpsDen: number; kbps: number; avcC: Buffer }
export interface VideoPacket { ptsUs: number; dtsUs: number; keyframe: boolean; data: Buffer }
export interface AudioConfig { sampleRate: number; channels: number; kbps: number; asc: Buffer }
export interface AudioPacket { ptsUs: number; data: Buffer }
export interface FeedConfig extends VideoConfig { feed: FeedId }
export interface FeedPacket extends VideoPacket { feed: FeedId }
/** RGBA: `height` rows of `stride` bytes. I420: the Y plane (`stride` = width) then U and V,
 *  each (width/2)×(height/2) — full-range BT.709. */
export interface Thumbnail { id: number; width: number; height: number; format: 'rgba' | 'i420'; stride: number; data: Buffer }

export interface Capabilities {
  engine: string;
  version: string;
  protocol: number;
  libobs: string;
  renderer: string;
  pid: number;
  video: { width: number; height: number; fps_num: number; fps_den: number; format: string; colorspace: string; range: string };
  audio: { sample_rate: number; channels: number };
  encoders: { h264_hardware: string[]; aac: string[]; x264: boolean };
  sources: string[];
}
export interface ReadyEvent extends Capabilities { event: 'ready' }
export interface ResultEvent { event: 'result'; req: number | string | null; cmd: string; ok: boolean; error?: string; data?: any }
export interface StatsEvent { event: 'stats'; [key: string]: any }
export interface ErrorEvent { event: 'error'; message: string; fatal?: boolean }
export interface LogEvent { event: 'log'; level: string; message: string }
export interface OutputEvent { event: 'output'; name: string; state: 'started' | 'stopped'; code?: number; error?: string }
export interface MediaEvent { event: 'media'; id: number; state: 'ended' }
export interface ShutdownEvent { event: 'shutdown'; reason: string }
export type EngineEvent = ReadyEvent | ResultEvent | StatsEvent | ErrorEvent | LogEvent | OutputEvent | MediaEvent | ShutdownEvent;

export type SourceType = 'camera' | 'media' | 'image' | 'color' | 'overlay' | 'pcm';
export interface AddSource {
  id: number;
  type: SourceType;
  // camera
  device?: string; device_name?: string; width?: number; height?: number; fps?: number; fast?: boolean; audio?: boolean;
  // media / image
  path?: string; loop?: boolean; hw_decode?: boolean; restart_on_activate?: boolean;
  /** media: 'as_is' passes an HLG or PQ clip's coded signal unchanged (for an app-supplied LUT); 'convert' (default): libobs converts it. */
  hdr?: 'as_is' | 'convert';
  // color
  color?: string;
  // overlay (also width/height)
  x?: number; y?: number; visible?: boolean; premultiplied?: boolean;
  // any source with sound
  volume?: number; muted?: boolean;
}

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

export function encodeFrame(type: number, payload: Buffer | Uint8Array = Buffer.alloc(0)): Buffer {
  const out = Buffer.allocUnsafe(HEADER_SIZE + payload.length);
  out.writeUInt32LE(payload.length, 0);
  out[4] = type;
  Buffer.from(payload.buffer, payload.byteOffset, payload.byteLength).copy(out, HEADER_SIZE);
  return out;
}

export function encodeCommand(cmd: Record<string, unknown>): Buffer {
  return encodeFrame(MsgType.COMMAND, Buffer.from(JSON.stringify(cmd), 'utf8'));
}

/** RGBA pixels for an overlay layer: a w x h rectangle at (x, y), rows `stride` bytes apart. */
export function encodeOverlay(id: number, x: number, y: number, w: number, h: number, stride: number,
  pixels: Uint8Array, flags = 0): Buffer {
  const need = h > 0 ? (h - 1) * stride + w * 4 : 0;
  if (pixels.length < need) throw new Error(`overlay: ${pixels.length} bytes, need ${need}`);
  const out = Buffer.allocUnsafe(HEADER_SIZE + 28 + need);
  out.writeUInt32LE(28 + need, 0);
  out[4] = MsgType.OVERLAY;
  out.writeUInt32LE(id, 5);
  out.writeUInt32LE(x, 9);
  out.writeUInt32LE(y, 13);
  out.writeUInt32LE(w, 17);
  out.writeUInt32LE(h, 21);
  out.writeUInt32LE(stride, 25);
  out.writeUInt32LE(flags, 29);
  Buffer.from(pixels.buffer, pixels.byteOffset, need).copy(out, HEADER_SIZE + 28);
  return out;
}

/** Interleaved stereo float32 at 48 kHz; `delayUs` = age of the first sample (for lip sync). */
export function encodePcm(id: number, interleaved: Float32Array, delayUs = 0): Buffer {
  const frames = Math.floor(interleaved.length / 2);
  const bytes = frames * 8;
  const out = Buffer.allocUnsafe(HEADER_SIZE + 16 + bytes);
  out.writeUInt32LE(16 + bytes, 0);
  out[4] = MsgType.PCM;
  out.writeUInt32LE(id, 5);
  out.writeUInt32LE(frames, 9);
  out.writeUInt32LE(Math.max(0, Math.min(0xffffffff, Math.round(delayUs))), 13);
  out.writeUInt32LE(0, 17);
  Buffer.from(interleaved.buffer, interleaved.byteOffset, bytes).copy(out, HEADER_SIZE + 16);
  return out;
}

/** Splits a byte stream into frames, however it is chunked. */
export class FrameParser {
  private chunks: Buffer[] = [];
  private size = 0;
  private header: { length: number; type: number } | null = null;
  private onFrame: (type: number, payload: Buffer) => void;

  constructor(onFrame: (type: number, payload: Buffer) => void) {
    this.onFrame = onFrame;
  }

  push(chunk: Buffer): void {
    if (chunk.length === 0) return;
    this.chunks.push(chunk);
    this.size += chunk.length;
    for (;;) {
      if (!this.header) {
        if (this.size < HEADER_SIZE) return;
        const h = this.take(HEADER_SIZE);
        const length = h.readUInt32LE(0);
        if (length > MAX_PAYLOAD) throw new Error(`protocol error: frame of ${length} bytes`);
        this.header = { length, type: h[4] };
      }
      if (this.size < this.header.length) return;
      const { type, length } = this.header;
      this.header = null;
      this.onFrame(type, length ? this.take(length) : Buffer.alloc(0));
    }
  }

  private take(n: number): Buffer {
    const first = this.chunks[0];
    if (first.length >= n) {
      const out = first.subarray(0, n);
      if (first.length === n) this.chunks.shift();
      else this.chunks[0] = first.subarray(n);
      this.size -= n;
      return out;
    }
    const out = Buffer.allocUnsafe(n);
    let off = 0;
    while (off < n) {
      const c = this.chunks[0];
      const k = Math.min(c.length, n - off);
      c.copy(out, off, 0, k);
      off += k;
      if (k === c.length) this.chunks.shift();
      else this.chunks[0] = c.subarray(k);
    }
    this.size -= n;
    return out;
  }
}

const readI64 = (b: Buffer, off: number) => Number(b.readBigInt64LE(off));

export function parseVideoConfig(p: Buffer): VideoConfig {
  return { width: p.readUInt16LE(0), height: p.readUInt16LE(2), fpsNum: p.readUInt32LE(4), fpsDen: p.readUInt32LE(8),
    kbps: p.readUInt32LE(12), avcC: p.subarray(16) };
}
export function parseVideoPacket(p: Buffer): VideoPacket {
  return { ptsUs: readI64(p, 0), dtsUs: readI64(p, 8), keyframe: p[16] === 1, data: p.subarray(17) };
}
export function parseAudioConfig(p: Buffer): AudioConfig {
  return { sampleRate: p.readUInt32LE(0), channels: p.readUInt16LE(4), kbps: p.readUInt32LE(8), asc: p.subarray(12) };
}
export function parseAudioPacket(p: Buffer): AudioPacket {
  return { ptsUs: readI64(p, 0), data: p.subarray(8) };
}
export function parseFeedConfig(p: Buffer): FeedConfig {
  return { feed: p[0] as FeedId, ...parseVideoConfig(p.subarray(1)) };
}
export function parseFeedPacket(p: Buffer): FeedPacket {
  return { feed: p[0] as FeedId, ...parseVideoPacket(p.subarray(1)) };
}
export function parseThumbnail(p: Buffer): Thumbnail {
  return { id: p.readUInt32LE(0), width: p.readUInt16LE(4), height: p.readUInt16LE(6), format: p[8] === 1 ? 'i420' : 'rgba',
    stride: p.readUInt32LE(12), data: p.subarray(16) };
}

// ---------------------------------------------------------------------------
// The client
// ---------------------------------------------------------------------------

export interface EngineClientOptions {
  /** Path to bin/skynat-engine (macOS) or skynat-engine.exe (Windows). */
  executable: string;
  /** Extra arguments, e.g. ['--width', '1920', '--height', '1080', '--fps', '30']. */
  args?: string[];
  env?: NodeJS.ProcessEnv;
  /** Restart after an unexpected exit (default true). */
  restart?: boolean;
  /** Delays between restart attempts; the last one repeats (default 0.5 s, 1 s, 2 s, 5 s, 10 s). */
  backoffMs?: number[];
  /** Give up after this many restarts without a minute of healthy running (default 5). */
  maxRestarts?: number;
  /** How long start() waits for the "ready" event (default 15 s). */
  startTimeoutMs?: number;
  /** Default timeout for command() (default 10 s). */
  commandTimeoutMs?: number;
  /** Stop sending PCM/overlays when this much is waiting to be written (default 32 MB). */
  maxStdinBacklog?: number;
  /** Receives every stderr line of the engine (its log). */
  onLog?: (line: string) => void;
}

export interface ExitInfo { code: number | null; signal: string | null; expected: boolean; restarting: boolean; attempt: number }

export interface EngineClientEvents {
  ready: (ev: ReadyEvent & { restarted: boolean }) => void;
  event: (ev: EngineEvent) => void;
  videoConfig: (cfg: VideoConfig) => void;
  videoPacket: (pkt: VideoPacket) => void;
  audioConfig: (cfg: AudioConfig) => void;
  audioPacket: (pkt: AudioPacket) => void;
  feedConfig: (cfg: FeedConfig) => void;
  feedPacket: (pkt: FeedPacket) => void;
  thumbnail: (t: Thumbnail) => void;
  exit: (info: ExitInfo) => void;
  restart: (info: { attempt: number; delayMs: number }) => void;
  failed: (info: { reason: string }) => void;
}

type Listener = (...args: any[]) => void;

export class CommandFailed extends Error {
  readonly cmd: string;
  constructor(cmd: string, message: string) {
    super(`${cmd}: ${message}`);
    this.cmd = cmd;
  }
}

export class EngineClient {
  private opts: Required<Omit<EngineClientOptions, 'onLog' | 'env' | 'args'>> & Pick<EngineClientOptions, 'onLog' | 'env' | 'args'>;
  private child: ChildProcess | null = null;
  private listeners = new Map<string, Set<Listener>>();
  private pending = new Map<number, { cmd: string; resolve: (v: any) => void; reject: (e: Error) => void; timer: ReturnType<typeof setTimeout> }>();
  private nextReq = 1;
  private stopping = false;
  private attempt = 0;
  private restartTimer: ReturnType<typeof setTimeout> | null = null;
  private healthyTimer: ReturnType<typeof setTimeout> | null = null;
  private readyWaiter: { resolve: (ev: ReadyEvent) => void; reject: (e: Error) => void; timer: ReturnType<typeof setTimeout> } | null = null;
  private stderrTail = '';
  private everStarted = false;
  private exitHook: (() => void) | null = null;
  /** Capabilities from the last "ready" event. */
  capabilities: Capabilities | null = null;

  constructor(options: EngineClientOptions) {
    this.opts = {
      executable: options.executable,
      args: options.args,
      env: options.env,
      onLog: options.onLog,
      restart: options.restart ?? true,
      backoffMs: options.backoffMs?.length ? options.backoffMs : [500, 1000, 2000, 5000, 10000],
      maxRestarts: options.maxRestarts ?? 5,
      startTimeoutMs: options.startTimeoutMs ?? 15000,
      commandTimeoutMs: options.commandTimeoutMs ?? 10000,
      maxStdinBacklog: options.maxStdinBacklog ?? 32 * 1024 * 1024,
    };
  }

  // ---- listeners -----------------------------------------------------------

  on<K extends keyof EngineClientEvents>(name: K, fn: EngineClientEvents[K]): this {
    let set = this.listeners.get(name);
    if (!set) this.listeners.set(name, (set = new Set()));
    set.add(fn as Listener);
    return this;
  }

  off<K extends keyof EngineClientEvents>(name: K, fn: EngineClientEvents[K]): this {
    this.listeners.get(name)?.delete(fn as Listener);
    return this;
  }

  private emit(name: keyof EngineClientEvents, ...args: unknown[]): void {
    const set = this.listeners.get(name);
    if (!set) return;
    for (const fn of [...set]) {
      try { fn(...args); } catch (e) { this.log(`[engine-client] listener for ${String(name)} threw: ${(e as Error).message}`); }
    }
  }

  private log(line: string): void {
    this.opts.onLog?.(line);
  }

  // ---- lifecycle -----------------------------------------------------------

  get running(): boolean { return !!this.child && this.child.exitCode === null && this.child.signalCode === null; }
  get pid(): number | undefined { return this.child?.pid; }
  /** Bytes waiting to be written to the engine's stdin. */
  get stdinBacklog(): number { return this.child?.stdin?.writableLength ?? 0; }

  /** Starts the engine and resolves with its "ready" event. */
  start(): Promise<ReadyEvent> {
    if (this.running) return Promise.reject(new Error('engine already running'));
    this.stopping = false;
    if (!this.exitHook) {
      // Last resort if the app quits without stop(): the engine also exits by
      // itself when its stdin closes, this only covers a hung engine.
      this.exitHook = () => { try { this.child?.kill('SIGKILL'); } catch { /* gone */ } };
      process.once('exit', this.exitHook);
    }
    return this.spawnEngine();
  }

  private spawnEngine(): Promise<ReadyEvent> {
    return new Promise<ReadyEvent>((resolve, reject) => {
      const spawnOpts: SpawnOptions = {
        stdio: ['pipe', 'pipe', 'pipe'],
        windowsHide: true,
        detached: false,
        env: this.opts.env ?? process.env,
      };
      let child: ChildProcess;
      try {
        child = spawn(this.opts.executable, this.opts.args ?? [], spawnOpts);
      } catch (e) {
        reject(e as Error);
        return;
      }
      this.child = child;
      this.stderrTail = '';
      const timer = setTimeout(() => {
        this.readyWaiter = null;
        reject(new Error(`engine did not become ready within ${this.opts.startTimeoutMs} ms`));
        try { child.kill('SIGKILL'); } catch { /* gone */ }
      }, this.opts.startTimeoutMs);
      this.readyWaiter = { resolve, reject, timer };

      const parser = new FrameParser((type, payload) => this.onFrame(type, payload));
      child.stdout!.on('data', (chunk: Buffer) => {
        try { parser.push(chunk); } catch (e) {
          this.log(`[engine-client] ${(e as Error).message}; killing the engine`);
          try { child.kill('SIGKILL'); } catch { /* gone */ }
        }
      });
      let partial = '';
      child.stderr!.on('data', (chunk: Buffer) => {
        const text = partial + chunk.toString('utf8');
        const lines = text.split('\n');
        partial = lines.pop() ?? '';
        for (const line of lines) {
          if (!line) continue;
          this.stderrTail = (this.stderrTail + line + '\n').slice(-4000);
          this.log(line);
        }
      });
      child.stdin!.on('error', () => { /* EPIPE when the engine dies; the exit handler deals with it */ });
      child.on('error', (err) => {
        this.log(`[engine-client] spawn error: ${err.message}`);
        if (this.readyWaiter) {
          clearTimeout(this.readyWaiter.timer);
          this.readyWaiter.reject(err);
          this.readyWaiter = null;
        }
      });
      child.on('exit', (code, signal) => this.onExit(child, code, signal));
    });
  }

  private onExit(child: ChildProcess, code: number | null, signal: NodeJS.Signals | null): void {
    if (this.child !== child) return;
    this.child = null;
    if (this.healthyTimer) { clearTimeout(this.healthyTimer); this.healthyTimer = null; }
    for (const [, p] of this.pending) {
      clearTimeout(p.timer);
      p.reject(new CommandFailed(p.cmd, 'engine exited'));
    }
    this.pending.clear();
    const expected = this.stopping;
    if (this.readyWaiter) {
      clearTimeout(this.readyWaiter.timer);
      this.readyWaiter.reject(new Error(`engine exited before ready (code ${code}, signal ${signal})\n${this.stderrTail}`));
      this.readyWaiter = null;
    }
    const canRestart = !expected && this.opts.restart && this.everStarted && this.attempt < this.opts.maxRestarts;
    this.emit('exit', { code, signal, expected, restarting: canRestart, attempt: this.attempt } as ExitInfo);
    if (expected) return;
    if (!canRestart) {
      if (!expected && this.everStarted) this.emit('failed', { reason: `engine exited (code ${code}, signal ${signal}) and will not be restarted` });
      return;
    }
    const list = this.opts.backoffMs;
    const delayMs = list[Math.min(this.attempt, list.length - 1)];
    this.attempt++;
    this.emit('restart', { attempt: this.attempt, delayMs });
    this.restartTimer = setTimeout(() => {
      this.restartTimer = null;
      if (this.stopping) return;
      this.spawnEngine().catch((e) => this.log(`[engine-client] restart failed: ${(e as Error).message}`));
    }, delayMs);
  }

  /** Asks the engine to shut down, waits for it to exit (kills it after `timeoutMs`). */
  async stop(timeoutMs = 5000): Promise<void> {
    this.stopping = true;
    if (this.restartTimer) { clearTimeout(this.restartTimer); this.restartTimer = null; }
    const child = this.child;
    if (!child) return;
    await new Promise<void>((resolve) => {
      const done = () => { clearTimeout(timer); resolve(); };
      const timer = setTimeout(() => {
        try { child.kill('SIGKILL'); } catch { /* gone */ }
        resolve();
      }, timeoutMs);
      child.once('exit', done);
      try {
        child.stdin!.write(encodeCommand({ cmd: 'shutdown' }));
        child.stdin!.end();
      } catch { /* already closing */ }
    });
    if (this.exitHook) { process.removeListener('exit', this.exitHook); this.exitHook = null; }
  }

  // ---- incoming ------------------------------------------------------------

  private onFrame(type: number, payload: Buffer): void {
    switch (type) {
      case MsgType.EVENT: {
        let ev: EngineEvent;
        try { ev = JSON.parse(payload.toString('utf8')); } catch { this.log('[engine-client] bad event JSON'); return; }
        this.onEvent(ev);
        return;
      }
      case MsgType.VIDEO_CONFIG: this.emit('videoConfig', parseVideoConfig(payload)); return;
      case MsgType.VIDEO_PACKET: this.emit('videoPacket', parseVideoPacket(payload)); return;
      case MsgType.AUDIO_CONFIG: this.emit('audioConfig', parseAudioConfig(payload)); return;
      case MsgType.AUDIO_PACKET: this.emit('audioPacket', parseAudioPacket(payload)); return;
      case MsgType.FEED_CONFIG: this.emit('feedConfig', parseFeedConfig(payload)); return;
      case MsgType.FEED_PACKET: this.emit('feedPacket', parseFeedPacket(payload)); return;
      case MsgType.THUMBNAIL: this.emit('thumbnail', parseThumbnail(payload)); return;
      default: this.log(`[engine-client] unknown message type ${type}`);
    }
  }

  private onEvent(ev: EngineEvent): void {
    if (ev.event === 'ready') {
      const restarted = this.everStarted;
      this.everStarted = true;
      const { event: _e, ...caps } = ev;
      this.capabilities = caps as Capabilities;
      if (this.readyWaiter) {
        clearTimeout(this.readyWaiter.timer);
        this.readyWaiter.resolve(ev);
        this.readyWaiter = null;
      }
      // A minute of healthy running resets the restart budget.
      if (this.healthyTimer) clearTimeout(this.healthyTimer);
      this.healthyTimer = setTimeout(() => { this.attempt = 0; this.healthyTimer = null; }, 60000);
      this.emit('ready', { ...ev, restarted });
    } else if (ev.event === 'result') {
      const req = typeof ev.req === 'number' ? ev.req : NaN;
      const p = this.pending.get(req);
      if (p) {
        this.pending.delete(req);
        clearTimeout(p.timer);
        if (ev.ok) p.resolve(ev.data ?? {});
        else p.reject(new CommandFailed(ev.cmd, ev.error ?? 'failed'));
      }
    }
    this.emit('event', ev);
  }

  // ---- outgoing ------------------------------------------------------------

  private write(buf: Buffer): boolean {
    const stdin = this.child?.stdin;
    if (!stdin || stdin.destroyed || !stdin.writable) return false;
    stdin.write(buf);
    return true;
  }

  /** Sends a command and resolves with its result data (rejects with CommandFailed). */
  command<T = any>(cmd: string, args: Record<string, unknown> = {}, timeoutMs = this.opts.commandTimeoutMs): Promise<T> {
    if (!this.running) return Promise.reject(new CommandFailed(cmd, 'engine not running'));
    const req = this.nextReq++;
    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(req);
        reject(new CommandFailed(cmd, `no answer within ${timeoutMs} ms`));
      }, timeoutMs);
      this.pending.set(req, { cmd, resolve, reject, timer });
      if (!this.write(encodeCommand({ ...args, cmd, req }))) {
        clearTimeout(timer);
        this.pending.delete(req);
        reject(new CommandFailed(cmd, 'engine stdin closed'));
      }
    });
  }

  /** Overlay pixels; returns false when dropped (engine gone or too much waiting). */
  sendOverlay(id: number, x: number, y: number, w: number, h: number, stride: number, rgba: Uint8Array, clear = false): boolean {
    if (this.stdinBacklog > this.opts.maxStdinBacklog) return false;
    return this.write(encodeOverlay(id, x, y, w, h, stride, rgba, clear ? OVERLAY_CLEAR : 0));
  }

  /** The app's mixer output: interleaved stereo float32, 48 kHz. */
  sendPcm(id: number, interleaved: Float32Array, delayUs = 0): boolean {
    if (this.stdinBacklog > this.opts.maxStdinBacklog) return false;
    return this.write(encodePcm(id, interleaved, delayUs));
  }

  // ---- typed helpers -------------------------------------------------------

  hello() { return this.command<Capabilities>('hello', { protocol: PROTOCOL_VERSION }); }
  configure(a: { width?: number; height?: number; fps?: number; fps_num?: number; fps_den?: number }) { return this.command('configure', a); }
  listDevices() { return this.command<{ cameras: { id: string; name: string }[]; camera_permission: string }>('list_devices'); }
  addSource(a: AddSource) { return this.command('add_source', a as unknown as Record<string, unknown>); }
  updateSource(a: Partial<AddSource> & { id: number }) { return this.command('update_source', a as unknown as Record<string, unknown>); }
  removeSource(id: number) { return this.command('remove_source', { id }); }
  setProgram(id: number | null, transition: 'cut' | 'fade' = 'cut', ms = 300) { return this.command('set_program', { id, transition, ms }); }
  setPreview(id: number | null) { return this.command('set_preview', { id }); }
  overlayVisible(id: number, visible: boolean) { return this.command('overlay_visible', { id, visible }); }
  media(id: number, action: 'play' | 'pause' | 'restart' | 'stop') { return this.command('media', { id, action }); }
  /** libobs's Apply LUT filter on a source, or on Program (`'program'`); an empty path takes it off. */
  lut(target: number | 'program', path: string, amount = 1) { return this.command('lut', target === 'program' ? { target, path, amount } : { id: target, path, amount }); }
  startOutput(a: { video_kbps?: number; audio_kbps?: number; keyint_sec?: number; encoder?: 'auto' | 'hardware' | 'x264' | string; encoder_settings?: Record<string, Record<string, unknown>> } = {}) {
    return this.command<{ encoder: string; settings: Record<string, unknown>; hardware: boolean; audio_encoder: string; video_kbps: number }>('start_output', a);
  }
  stopOutput() { return this.command('stop_output', {}, 15000); }
  setBitrate(kbps: number) { return this.command('set_bitrate', { kbps }); }
  previewFeed(a: { on: boolean; width?: number; height?: number; fps?: number; preview_fps?: number; kbps?: number; program?: boolean; preview?: boolean }) {
    return this.command('preview_feed', a, 15000);
  }
  /** `format` i420: 1.5 bytes a pixel instead of 4; `only_changes`: an input's thumbnail is sent
   *  only when its picture changed since the last one sent (a still picture costs nothing). */
  thumbnails(a: { on: boolean; width?: number; height?: number; fps?: number; format?: 'rgba' | 'i420'; only_changes?: boolean }) { return this.command('thumbnails', a); }
  stats(intervalMs?: number) { return this.command('stats', intervalMs === undefined ? {} : { interval_ms: intervalMs }); }

  // ---- orphans ---------------------------------------------------------------

  /**
   * Kills engines started from `executable` whose parent app is gone (left
   * behind by a crash). Call once at app start, before start(). Resolves
   * with the number of processes killed.
   */
  static killOrphans(executable: string): Promise<number> {
    return new Promise((resolve) => {
      const alive = (pid: number) => { try { process.kill(pid, 0); return true; } catch (e) { return (e as NodeJS.ErrnoException).code === 'EPERM'; } };
      const finish = (rows: { pid: number; ppid: number; path: string }[]) => {
        let killed = 0;
        const want = executable.replace(/\\/g, '/').toLowerCase();
        for (const r of rows) {
          if (r.pid === process.pid) continue;
          if (r.path.replace(/\\/g, '/').toLowerCase() !== want) continue;
          const orphan = r.ppid <= 1 || !alive(r.ppid);
          if (!orphan) continue;
          try { process.kill(r.pid, 'SIGKILL'); killed++; } catch { /* already gone */ }
        }
        resolve(killed);
      };
      if (process.platform === 'win32') {
        const exe = executable.split(/[\\/]/).pop() ?? 'skynat-engine.exe';
        const ps = `Get-CimInstance Win32_Process -Filter "Name='${exe.replace(/'/g, "''")}'" | ForEach-Object { "$($_.ProcessId)|$($_.ParentProcessId)|$($_.ExecutablePath)" }`;
        execFile('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', ps], { windowsHide: true, timeout: 15000 }, (err, stdout) => {
          if (err) { resolve(0); return; }
          finish(String(stdout).split(/\r?\n/).filter(Boolean).map((l) => {
            const [pid, ppid, ...rest] = l.split('|');
            return { pid: Number(pid), ppid: Number(ppid), path: rest.join('|').trim() };
          }));
        });
      } else {
        execFile('ps', ['-axww', '-o', 'pid=,ppid=,command='], { timeout: 10000 }, (err, stdout) => {
          if (err) { resolve(0); return; }
          finish(String(stdout).split('\n').filter(Boolean).map((l) => {
            const m = l.trim().match(/^(\d+)\s+(\d+)\s+(.*)$/);
            if (!m) return { pid: 0, ppid: 0, path: '' };
            // command = executable + args; the engine path has no arguments that look like it
            const cmd = m[3];
            const path = cmd.startsWith(executable) ? executable : cmd.split(' ')[0];
            return { pid: Number(m[1]), ppid: Number(m[2]), path };
          }));
        });
      }
    });
  }
}
