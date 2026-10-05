// telosnex_audio web backing store (ADR D11, D14). A dedicated worker.
//
// Streamed `.all` tracks: Dart sends every write here as well as to the
// worklet. Each complete 1 s chunk goes to IndexedDB (memory if IndexedDB
// fails). MP3 tracks: the worker keeps the bytes and decodes chunks with
// its own instance of the wasm core.
//
// The worklet asks for chunks over a MessagePort: {t:'need', id, k}. The
// answer is {t:'chunk', id, k, ch, pcm} or {t:'fail', id}.
'use strict';

let x = null; // wasm exports (MP3 only)
let port = null;
let session = '';
let db = null;
const memory = new Map(); // key string -> Int16Array, when IndexedDB fails
const tracks = new Map();

const DB = 'telosnex_audio';
const CHUNKS = 'chunks';
const SESSIONS = 'sessions';
const STALE_MS = 60000;

self.onmessage = (e) => {
  const m = e.data;
  switch (m.t) {
    case 'init':
      init(m);
      break;
    case 'port':
      port = m.port;
      port.onmessage = (ev) => need(ev.data.id, ev.data.k);
      break;
    case 'pcm':
      tracks.set(m.id, {
        mp3: false, rate: m.rate, ch: m.ch, cur: new Int16Array(m.rate * m.ch),
        fill: 0, k: 0, eos: false, stored: new Set(), waiting: new Set(),
      });
      break;
    case 'write':
      write(m.id, m.pcm);
      break;
    case 'eos':
      eos(m.id);
      break;
    case 'mp3':
      openMp3(m.id, m.bytes);
      break;
    case 'dispose':
      dispose(m.id);
      break;
    case 'close':
      for (const id of [...tracks.keys()]) dispose(id);
      deleteSession(session);
      self.close();
      break;
  }
};

function init(m) {
  session = m.session;
  const module = new WebAssembly.Module(m.wasm);
  const instance = new WebAssembly.Instance(module, {
    env: {
      emscripten_notify_memory_growth: () => {},
      __syscall_unlinkat: () => -1,
      __syscall_rmdir: () => -1,
    },
    wasi_snapshot_preview1: { fd_close: () => 8, fd_pread: () => 8, fd_pwrite: () => 8 },
  });
  x = instance.exports;
  x._initialize();
  openDb();
}

function openDb() {
  let req;
  try {
    req = indexedDB.open(DB, 1);
  } catch (_) {
    return;
  }
  req.onupgradeneeded = () => {
    req.result.createObjectStore(CHUNKS);
    req.result.createObjectStore(SESSIONS);
  };
  req.onsuccess = () => {
    db = req.result;
    heartbeat();
    setInterval(heartbeat, 10000);
    cleanStale();
  };
  req.onerror = () => { db = null; };
}

function heartbeat() {
  if (!db) return;
  try {
    db.transaction(SESSIONS, 'readwrite').objectStore(SESSIONS).put(Date.now(), session);
  } catch (_) {}
}

// Chunks of sessions whose page is gone (ADR I11).
function cleanStale() {
  const tx = db.transaction(SESSIONS, 'readonly');
  const req = tx.objectStore(SESSIONS).openCursor();
  const stale = [];
  req.onsuccess = () => {
    const c = req.result;
    if (c) {
      if (c.key !== session && Date.now() - c.value > STALE_MS) stale.push(c.key);
      c.continue();
    } else {
      for (const s of stale) deleteSession(s);
    }
  };
}

function deleteSession(s) {
  if (!db) return;
  try {
    const tx = db.transaction([CHUNKS, SESSIONS], 'readwrite');
    tx.objectStore(CHUNKS).delete(IDBKeyRange.bound([s], [s, []]));
    tx.objectStore(SESSIONS).delete(s);
  } catch (_) {}
}

function key(id, k) {
  return [session, id, k];
}

function store(id, t, k, pcm) {
  const done = () => {
    t.stored.add(k);
    if (t.waiting.delete(k)) need(id, k);
  };
  if (!db) {
    memory.set(key(id, k).join('/'), pcm);
    done();
    return;
  }
  try {
    const tx = db.transaction(CHUNKS, 'readwrite');
    tx.objectStore(CHUNKS).put(pcm.buffer, key(id, k));
    tx.oncomplete = done;
    tx.onerror = tx.onabort = () => {
      memory.set(key(id, k).join('/'), pcm);
      done();
    };
  } catch (_) {
    memory.set(key(id, k).join('/'), pcm);
    done();
  }
}

function write(id, pcm) {
  const t = tracks.get(id);
  if (!t || t.mp3 || t.eos) return;
  let i = 0;
  while (i < pcm.length) {
    const n = Math.min(pcm.length - i, t.cur.length - t.fill);
    t.cur.set(pcm.subarray(i, i + n), t.fill);
    t.fill += n;
    i += n;
    if (t.fill === t.cur.length) {
      store(id, t, t.k, t.cur);
      t.k++;
      t.cur = new Int16Array(t.rate * t.ch);
      t.fill = 0;
    }
  }
}

function eos(id) {
  const t = tracks.get(id);
  if (!t || t.mp3 || t.eos) return;
  t.eos = true;
  if (t.fill > 0) store(id, t, t.k, t.cur.slice(0, t.fill));
}

function send(id, k, ch, pcm) {
  port.postMessage({ t: 'chunk', id, k, ch, pcm }, [pcm.buffer]);
}

function need(id, k) {
  const t = tracks.get(id);
  if (!t || !port) return;
  if (t.mp3) {
    const frames = Math.min(t.rate, t.frames - k * t.rate);
    if (frames <= 0) return;
    const p = x.tsnxw_alloc(frames * t.ch * 2);
    if (x.tsnxw_mp3_read(t.handle, k * t.rate, frames, p) !== 0) {
      x.tsnxw_free(p);
      port.postMessage({ t: 'fail', id });
      return;
    }
    const pcm = new Int16Array(x.memory.buffer, p, frames * t.ch).slice();
    x.tsnxw_free(p);
    send(id, k, t.ch, pcm);
    return;
  }
  if (!t.stored.has(k)) {
    // Asked before the write reached this worker.
    t.waiting.add(k);
    return;
  }
  const mem = memory.get(key(id, k).join('/'));
  if (mem) {
    send(id, k, t.ch, mem.slice());
    return;
  }
  const req = db.transaction(CHUNKS, 'readonly').objectStore(CHUNKS).get(key(id, k));
  req.onsuccess = () => {
    if (req.result) send(id, k, t.ch, new Int16Array(req.result));
    else port.postMessage({ t: 'fail', id });
  };
  req.onerror = () => port.postMessage({ t: 'fail', id });
}

function openMp3(id, bytes) {
  const src = new Uint8Array(bytes);
  const p = x.tsnxw_alloc(src.length);
  new Uint8Array(x.memory.buffer, p, src.length).set(src);
  const h = x.tsnxw_mp3_open(p, src.length); // takes p
  if (!h) {
    self.postMessage({ t: 'mp3', id, error: 'not a supported MP3' });
    return;
  }
  const t = {
    mp3: true, handle: h, rate: x.tsnxw_mp3_rate(h), ch: x.tsnxw_mp3_channels(h),
    frames: x.tsnxw_mp3_frames(h),
  };
  tracks.set(id, t);
  self.postMessage({ t: 'mp3', id, rate: t.rate, ch: t.ch, frames: t.frames });
}

function dispose(id) {
  const t = tracks.get(id);
  if (!t) return;
  tracks.delete(id);
  if (t.mp3) {
    x.tsnxw_mp3_close(t.handle);
    return;
  }
  for (const k of t.stored) memory.delete(key(id, k).join('/'));
  if (!db) return;
  try {
    db.transaction(CHUNKS, 'readwrite').objectStore(CHUNKS)
      .delete(IDBKeyRange.bound([session, id], [session, id, []]));
  } catch (_) {}
}
