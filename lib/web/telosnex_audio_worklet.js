// telosnex_audio web engine (ADR D14).
//
// This file loads two ways:
//  - as an AudioWorklet module: it registers 'telosnex-audio' (the engine)
//    and 'telosnex-tap' (a test probe that records what it passes through);
//  - as a classic script on the main thread: it defines globalThis.TsnxCore,
//    which ManualDevice engines (tests) drive directly.
// Do not use import or export here; both loaders must accept the file.
(function () {
  'use strict';

  const BLOCK = 480; // 10 ms at 48 kHz, the mix rate

  // The wasm core and its message protocol. `post(msg, transfer)` sends to
  // Dart. Messages from Dart go to handle(). See lib/src/engine_web.dart.
  class TsnxCore {
    // `rate`: the device rate (a multiple of 100). Blocks are rate / 100.
    constructor(module, post, rate) {
      this.post = post;
      this.buf = null;
      const instance = new WebAssembly.Instance(module, {
        env: {
          emscripten_notify_memory_growth: () => {},
          __syscall_unlinkat: () => -1,
          __syscall_rmdir: () => -1,
        },
        // The native spill file code is linked but never runs on web.
        wasi_snapshot_preview1: {
          fd_close: () => 8,
          fd_pread: () => 8,
          fd_pwrite: () => 8,
        },
      });
      this.x = instance.exports;
      this.x._initialize();
      this.x.tsnxw_init();
      this.rate = rate || 48000;
      if (this.x.tsnxw_set_rate(this.rate) !== 0) {
        throw new Error('telosnex_audio: unsupported device rate ' + this.rate);
      }
      this.block = this.rate / 100;
      this.storage = null; // MessagePort to the storage worker
      this.delayNs = 0;
      this.nowNs = 0; // the render clock at the last block
      this.captureRate = 0;
      this.capture = new Float32Array(this.block);
      this.captureFill = 0;
      this.captureAtNs = 0;
    }

    views() {
      const b = this.x.memory.buffer;
      if (b !== this.buf) {
        this.buf = b;
        this.f32 = new Float32Array(b);
        this.f64 = new Float64Array(b);
        this.i16 = new Int16Array(b);
      }
    }

    handle(m) {
      const x = this.x;
      let r = 0;
      switch (m.t) {
        case 'create':
          r = x.tsnxw_track_create(m.id, m.rate, m.ch, m.retention);
          break;
        case 'whole':
          r = x.tsnxw_track_create_whole(m.id, m.rate, m.ch, m.frames);
          break;
        case 'write': {
          const pcm = m.pcm;
          const p = x.tsnxw_alloc(pcm.length * 2);
          this.views();
          this.i16.set(pcm, p >> 1);
          r = x.tsnxw_write(m.id, p, pcm.length / m.ch);
          x.tsnxw_free(p);
          break;
        }
        case 'eos':
          r = x.tsnxw_end_of_stream(m.id);
          break;
        case 'cmd':
          r = x.tsnxw_command(m.k, m.id, m.i, m.d);
          break;
        case 'dispose':
          x.tsnxw_dispose(m.id);
          break;
        case 'delay':
          this.delayNs = m.ns;
          break;
        case 'idle':
          // The context is not rendering: apply commands and publish now.
          this.idle(this.nowNs);
          break;
        case 'restart':
          // The output stopped; the audio in its buffer was not heard
          // (ADR I10). Playing tracks go back to the heard position.
          x.tsnxw_output_restart(this.nowNs);
          break;
        case 'capture':
          this.captureRate = m.rate;
          this.captureFill = 0;
          break;
        case 'storage':
          this.storage = m.port;
          this.storage.onmessage = (e) => this.onStorage(e.data);
          break;
      }
      if (r < 0) this.post({ t: 'err', id: m.id, op: m.t, code: r });
    }

    onStorage(m) {
      const x = this.x;
      if (m.t === 'chunk') {
        const pcm = m.pcm;
        const p = x.tsnxw_alloc(pcm.length * 2);
        this.views();
        this.i16.set(pcm, p >> 1);
        x.tsnxw_deliver(m.id, m.k, p, pcm.length / m.ch);
        x.tsnxw_free(p);
      } else if (m.t === 'fail') {
        x.tsnxw_fail(m.id);
      }
    }

    // One block. Returns the planar stereo output, `block` frames per
    // channel (a view; copy it before the next call into the module).
    render(mixNs, nowNs) {
      this.nowNs = nowNs;
      const p = this.x.tsnxw_render(mixNs, this.delayNs, nowNs);
      this.flush(nowNs);
      this.views();
      return this.f32.subarray(p >> 2, (p >> 2) + 2 * this.block);
    }

    idle(nowNs) {
      this.x.tsnxw_idle(nowNs);
      this.flush(nowNs);
    }

    // Sends states, events, and chunk requests from the last call.
    flush(nowNs) {
      const x = this.x;
      this.views();
      const ns = x.tsnxw_state_count();
      const ne = x.tsnxw_event_count();
      const nr = x.tsnxw_request_count();
      if (ns > 0 || ne > 0) {
        const s = this.f64.slice(x.tsnxw_states() >> 3, (x.tsnxw_states() >> 3) + ns * 9);
        const e = this.f64.slice(x.tsnxw_events() >> 3, (x.tsnxw_events() >> 3) + ne * 3);
        this.post({ t: 'tick', now: nowNs, s, e }, [s.buffer, e.buffer]);
      }
      if (nr > 0) {
        const q = x.tsnxw_requests() >> 3;
        for (let i = 0; i < nr; i++) {
          const id = this.f64[q + 2 * i];
          const k = this.f64[q + 2 * i + 1];
          if (this.storage) this.storage.postMessage({ t: 'need', id, k });
          else x.tsnxw_fail(id);
        }
      }
      x.tsnxw_clear();
    }

    // Mono float samples from the microphone at the device rate. `atNs`:
    // when the first one was captured, on the render clock.
    captureIn(samples, atNs) {
      if (this.captureRate === 0) return;
      let i = 0;
      while (i < samples.length) {
        if (this.captureFill === 0) {
          this.captureAtNs = atNs + (i * 1e9) / this.rate;
        }
        const n = Math.min(samples.length - i, this.block - this.captureFill);
        this.capture.set(samples.subarray(i, i + n), this.captureFill);
        this.captureFill += n;
        i += n;
        if (this.captureFill === this.block) {
          this.captureFill = 0;
          const x = this.x;
          this.views();
          this.f32.set(this.capture, x.tsnxw_capture_in() >> 2);
          const p = x.tsnxw_capture(this.captureRate) >> 1;
          this.views();
          const pcm = this.i16.slice(p, p + this.captureRate / 100);
          this.post(
            { t: 'cap', at: this.captureAtNs, rate: this.captureRate, pcm },
            [pcm.buffer],
          );
        }
      }
    }

    // ManualDevice (device rate 48 kHz): renders `blocks` blocks,
    // `channels` (1 or 2) interleaved PCM16. `captureIn`: 480 mono PCM16
    // samples per block.
    renderManual(blocks, channels, startNs, captureIn) {
      const out = new Int16Array(blocks * BLOCK * channels);
      const f = new Float32Array(BLOCK);
      let now = startNs;
      for (let b = 0; b < blocks; b++) {
        const v = this.render(now, now);
        const o = b * BLOCK * channels;
        for (let i = 0; i < BLOCK; i++) {
          const l = v[i];
          const r = v[BLOCK + i];
          if (channels === 2) {
            out[o + 2 * i] = toS16(l);
            out[o + 2 * i + 1] = toS16(r);
          } else {
            out[o + i] = toS16((l + r) / 2);
          }
        }
        if (captureIn) {
          for (let i = 0; i < BLOCK; i++) f[i] = captureIn[b * BLOCK + i] / 32768;
          this.captureIn(f, now);
        }
        now += 1e7;
      }
      return out;
    }
  }

  function toS16(v) {
    const s = Math.round(v * 32768);
    return s > 32767 ? 32767 : s < -32768 ? -32768 : s;
  }

  globalThis.TsnxCore = TsnxCore;
  if (typeof registerProcessor !== 'function') return;

  // The engine node: input 0 is the microphone (optional), output 0 is
  // stereo. Blocks are 480 frames; quanta are 128, so a block is rendered
  // when the last one runs out.
  class TsnxProcessor extends AudioWorkletProcessor {
    constructor(options) {
      super();
      const module = new WebAssembly.Module(options.processorOptions.wasm);
      this.core = new TsnxCore(
        module, (m, tr) => this.port.postMessage(m, tr || []), sampleRate);
      this.port.onmessage = (e) => this.core.handle(e.data);
      this.block = this.core.block;
      this.left = new Float32Array(this.block);
      this.right = new Float32Array(this.block);
      this.pos = this.block;
      this.mono = new Float32Array(128);
    }

    process(inputs, outputs) {
      const out = outputs[0];
      const left = out[0];
      const right = out.length > 1 ? out[1] : null;
      const n = left.length;
      let w = 0;
      while (w < n) {
        const b = this.block;
        if (this.pos >= b) {
          const mixNs = ((currentFrame + w) * 1e9) / sampleRate;
          const v = this.core.render(mixNs, (currentFrame * 1e9) / sampleRate);
          this.left.set(v.subarray(0, b));
          this.right.set(v.subarray(b, 2 * b));
          this.pos = 0;
        }
        const m = Math.min(n - w, b - this.pos);
        left.set(this.left.subarray(this.pos, this.pos + m), w);
        if (right) right.set(this.right.subarray(this.pos, this.pos + m), w);
        w += m;
        this.pos += m;
      }
      const mic = inputs[0];
      if (mic && mic.length > 0 && this.core.captureRate !== 0) {
        const len = mic[0].length;
        if (this.mono.length !== len) this.mono = new Float32Array(len);
        if (mic.length === 1) {
          this.mono.set(mic[0]);
        } else {
          for (let i = 0; i < len; i++) {
            let a = 0;
            for (let c = 0; c < mic.length; c++) a += mic[c][i];
            this.mono[i] = a / mic.length;
          }
        }
        this.core.captureIn(this.mono, (currentFrame * 1e9) / sampleRate);
      }
      return true;
    }
  }
  registerProcessor('telosnex-audio', TsnxProcessor);

  // Test probe: passes input 0 to output 0 and, while recording, posts the
  // left channel in 100 ms pieces with the context frame of the first one.
  class TsnxTap extends AudioWorkletProcessor {
    constructor() {
      super();
      this.on = false;
      this.buf = new Float32Array(4800);
      this.fill = 0;
      this.start = 0;
      this.port.onmessage = (e) => {
        this.on = e.data.on;
        this.fill = 0;
      };
    }

    process(inputs, outputs) {
      const input = inputs[0];
      const out = outputs[0];
      for (let c = 0; c < out.length; c++) {
        if (input.length > c) out[c].set(input[c]);
        else if (input.length > 0) out[c].set(input[0]);
        else out[c].fill(0);
      }
      if (!this.on) return true;
      const src = input.length > 0 ? input[0] : null;
      const n = out[0].length;
      for (let i = 0; i < n; i++) {
        if (this.fill === 0) this.start = currentFrame + i;
        this.buf[this.fill++] = src ? src[i] : 0;
        if (this.fill === this.buf.length) {
          const piece = this.buf.slice();
          this.port.postMessage({ frame: this.start, pcm: piece }, [piece.buffer]);
          this.fill = 0;
        }
      }
      return true;
    }
  }
  registerProcessor('telosnex-tap', TsnxTap);
})();
