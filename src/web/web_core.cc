// The web engine core (ADR D14): the Track core compiled to WebAssembly.
//
// An AudioWorklet runs it (lib/web/telosnex_audio_worklet.js), so there
// is one thread: message handlers and process() take turns. What the native
// engine does on its control and notifier threads happens here after each
// render, in the same thread. A storage worker (telosnex_audio_storage.js)
// runs a second instance for MP3 decoding only (the tsnxw_mp3_* functions).
//
// 64-bit values cross the boundary as doubles. Frames and nanoseconds stay
// below 2^53.
#include <emscripten/emscripten.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "block_resampler.h"
#include "minimp3_ex.h"
#include "track.h"
#include "track_store.h"
#include "rtc_base/checks.h"
#include "util/retirer.h"

namespace tsnx {
namespace {

constexpr int kMaxTracks = 8;
constexpr int kBlock = Track::kOutFrames;  // 480
constexpr int kMaxEvents = 256;
constexpr int kMaxRequests = 256;
constexpr int kStateWords = 9;  // id + TrackStateWords

enum Error : int32_t {
  kOk = 0,
  kErrInvalidArgument = -1,
  kErrNoFreeTrack = -2,
  kErrNoTrack = -3,
  kErrUnsupportedFormat = -5,
  kErrQueueFull = -6,
};

enum CommandKind : int32_t {
  kPlay = 0,
  kPause = 1,
  kSeek = 2,
  kRate = 3,
  kGain = 4,
  kFlush = 5,
};

struct Command {
  int32_t kind;
  int32_t id;
  int64_t i;
  double d;
};

class Core : public EventSink {
 public:
  Track* tracks[kMaxTracks] = {};
  int64_t last_chunk[kMaxTracks] = {};
  bool dirty[kMaxTracks] = {};
  Retirer retirer;

  Command commands[256];
  int n_commands = 0;

  // Read by the host after each call that can produce them.
  double events[kMaxEvents * 3];
  int n_events = 0;
  double states[kMaxTracks * kStateWords];
  int n_states = 0;
  double requests[kMaxRequests * 2];
  int n_requests = 0;

  // The mix is 48 kHz. The browser can run at another rate; then the
  // output is resampled to it.
  int out_rate = 48000;
  int out_frames = kBlock;
  float mix[kBlock * 2];  // interleaved stereo, int16 scale
  std::vector<float> resampled;
  std::vector<float> out;  // planar: out_frames left, then right; -1..1
  std::unique_ptr<BlockResampler> out_rs;
  int16_t scratch[kBlock * 2];
  std::vector<int64_t> asked;

  // Capture: mono float (-1..1) at the device rate in, PCM16 out at the
  // asked rate.
  std::vector<float> cap_in;
  std::vector<float> cap_f;
  std::vector<float> cap_r;
  std::vector<int16_t> cap_out;
  std::unique_ptr<BlockResampler> cap_rs;
  int cap_from = 0;
  int cap_to = 0;

  Core() { SetOutputRate(48000); }

  void SetOutputRate(int rate) {
    out_rate = rate;
    out_frames = rate / 100;
    out.assign(static_cast<size_t>(out_frames) * 2, 0.f);
    resampled.assign(static_cast<size_t>(out_frames) * 2, 0.f);
    out_rs = rate == 48000
                 ? nullptr
                 : std::make_unique<BlockResampler>(48000, rate, 2);
    cap_in.assign(static_cast<size_t>(out_frames), 0.f);
  }

  void PushRt(const TrackEvent& e) override {
    if (n_events >= kMaxEvents) return;
    events[n_events * 3] = e.kind;
    events[n_events * 3 + 1] = e.track_id;
    events[n_events * 3 + 2] = static_cast<double>(e.value);
    ++n_events;
  }

  int Slot(int32_t id) const {
    for (int s = 0; s < kMaxTracks; ++s)
      if (tracks[s] && tracks[s]->id() == id) return s;
    return -1;
  }
  Track* Find(int32_t id) const {
    const int s = Slot(id);
    return s < 0 ? nullptr : tracks[s];
  }

  int32_t Add(int32_t id, std::unique_ptr<TrackStore> store) {
    if (Slot(id) >= 0) return kErrInvalidArgument;
    for (int s = 0; s < kMaxTracks; ++s) {
      if (tracks[s]) continue;
      tracks[s] = new Track(id, std::move(store));
      last_chunk[s] = -1;
      dirty[s] = true;
      return kOk;
    }
    return kErrNoFreeTrack;
  }

  void ApplyCommands() {
    for (int n = 0; n < n_commands; ++n) {
      const Command& c = commands[n];
      const int s = Slot(c.id);
      if (s < 0) continue;
      Track* t = tracks[s];
      switch (c.kind) {
        case kPlay: t->Play(); break;
        case kPause: t->Pause(); break;
        case kSeek: t->Seek(c.i); break;
        case kRate: t->SetRate(c.d); break;
        case kGain: t->SetGain(c.d, c.i * t->sample_rate() / 1000); break;
        case kFlush: t->Flush(c.i); break;
      }
      dirty[s] = true;
    }
    n_commands = 0;
  }

  // The native control thread's pass: move windows, collect requests.
  void Maintain() {
    for (int s = 0; s < kMaxTracks; ++s) {
      Track* t = tracks[s];
      if (!t) continue;
      TrackStore& st = t->store();
      const int64_t chunk = t->playhead() / st.chunk_frames();
      if (!dirty[s] && chunk == last_chunk[s]) continue;
      dirty[s] = false;
      last_chunk[s] = chunk;
      st.Maintain(t->playhead(), retirer);
      if (!st.external()) continue;
      asked.clear();
      st.TakeRequests(&asked);
      for (int64_t k : asked) {
        if (n_requests >= kMaxRequests) break;
        requests[n_requests * 2] = t->id();
        requests[n_requests * 2 + 1] = static_cast<double>(k);
        ++n_requests;
      }
    }
    retirer.Reclaim();
  }

  void Publish(int64_t now_ns) {
    n_states = 0;
    for (Track* t : tracks) {
      if (!t) continue;
      t->Publish(now_ns, *this);
      const TrackStateWords w = t->State();
      double* d = states + n_states * kStateWords;
      d[0] = t->id();
      d[1] = static_cast<double>(w.status);
      d[2] = static_cast<double>(w.position);
      d[3] = static_cast<double>(w.sampled_at_ns);
      d[4] = w.rate;
      d[5] = static_cast<double>(w.written);
      d[6] = static_cast<double>(w.duration);
      d[7] = static_cast<double>(w.mixer_position);
      d[8] = static_cast<double>(w.underruns);
      ++n_states;
    }
  }

  void Render(int64_t mix_ns, int64_t delay_ns, int64_t now_ns) {
    retirer.BeginBlock();
    ApplyCommands();
    std::memset(mix, 0, sizeof(mix));
    for (Track* t : tracks) {
      if (!t) continue;
      if (!t->Render(scratch, mix_ns, delay_ns, *this)) continue;
      if (t->channels() == 2) {
        for (int i = 0; i < kBlock * 2; ++i) mix[i] += scratch[i];
      } else {
        for (int i = 0; i < kBlock; ++i) {
          mix[2 * i] += scratch[i];
          mix[2 * i + 1] += scratch[i];
        }
      }
    }
    const float* src = mix;
    if (out_rs) {
      out_rs->Process(mix, resampled.data());
      src = resampled.data();
    }
    constexpr float kScale = 1.0f / 32768.0f;
    for (int i = 0; i < out_frames; ++i) {
      out[i] = std::clamp(src[2 * i] * kScale, -1.0f, 1.0f);
      out[out_frames + i] = std::clamp(src[2 * i + 1] * kScale, -1.0f, 1.0f);
    }
    Publish(now_ns);
    retirer.EndBlock();
    Maintain();
  }
};

Core* g_core = nullptr;

struct Mp3File {
  std::vector<uint8_t> bytes;
  mp3dec_ex_t dec;
};

}  // namespace
}  // namespace tsnx

using tsnx::g_core;

// WebRTC's RTC_CHECK failure (built with RTC_DISABLE_CHECK_MSG). No logging
// in the worklet: trap, and the host reports the processor error.
namespace webrtc::webrtc_checks_impl {
void FatalLog(const char* /*file*/, int /*line*/) { __builtin_trap(); }
}  // namespace webrtc::webrtc_checks_impl

extern "C" {

EMSCRIPTEN_KEEPALIVE void* tsnxw_alloc(int32_t bytes) {
  return std::malloc(static_cast<size_t>(bytes));
}
EMSCRIPTEN_KEEPALIVE void tsnxw_free(void* p) { std::free(p); }

EMSCRIPTEN_KEEPALIVE int32_t tsnxw_init() {
  if (!g_core) g_core = new tsnx::Core;
  return 0;
}

// retention: 0 all (external backing), 1 unplayed.
EMSCRIPTEN_KEEPALIVE int32_t tsnxw_track_create(int32_t id, int32_t rate,
                                                int32_t channels,
                                                int32_t retention) {
  if (rate < 8000 || rate > 48000 || rate % 100 != 0 || channels < 1 ||
      channels > 2)
    return tsnx::kErrUnsupportedFormat;
  std::unique_ptr<tsnx::TrackStore> store =
      retention == 0
          ? tsnx::TrackStore::External(rate, channels, -1)
          : std::make_unique<tsnx::TrackStore>(
                rate, channels, tsnx::Retention::kUnplayed, std::string());
  return g_core->Add(id, std::move(store));
}

// A whole track at the host (MP3): `frames` long, nothing in memory yet.
EMSCRIPTEN_KEEPALIVE int32_t tsnxw_track_create_whole(int32_t id,
                                                      int32_t rate,
                                                      int32_t channels,
                                                      double frames) {
  if (rate < 8000 || rate > 48000 || rate % 100 != 0 || channels < 1 ||
      channels > 2 || !(frames > 0))
    return tsnx::kErrUnsupportedFormat;
  return g_core->Add(id, tsnx::TrackStore::External(
                             rate, channels, static_cast<int64_t>(frames)));
}

EMSCRIPTEN_KEEPALIVE double tsnxw_write(int32_t id, const int16_t* pcm,
                                        double frames) {
  const int s = g_core->Slot(id);
  if (s < 0) return tsnx::kErrNoTrack;
  tsnx::Track* t = g_core->tracks[s];
  const int64_t r = t->store().Write(pcm, static_cast<int64_t>(frames),
                                     t->playhead(), g_core->retirer);
  g_core->dirty[s] = true;
  return static_cast<double>(r);
}

EMSCRIPTEN_KEEPALIVE int32_t tsnxw_end_of_stream(int32_t id) {
  const int s = g_core->Slot(id);
  if (s < 0) return tsnx::kErrNoTrack;
  g_core->tracks[s]->store().EndOfStream();
  g_core->dirty[s] = true;
  return 0;
}

// Applied at the start of the next block (ADR D10). A flush cuts at the
// audio written when it was asked.
EMSCRIPTEN_KEEPALIVE int32_t tsnxw_command(int32_t kind, int32_t id, double i,
                                           double d) {
  tsnx::Track* t = g_core->Find(id);
  if (!t) return tsnx::kErrNoTrack;
  if (kind == tsnx::kRate && !(d >= 0.5 && d <= 3.0))
    return tsnx::kErrInvalidArgument;
  if (kind == tsnx::kGain && (!(d >= 0.0 && d <= 4.0) || i < 0))
    return tsnx::kErrInvalidArgument;
  if (g_core->n_commands >= 256) return tsnx::kErrQueueFull;
  int64_t v = static_cast<int64_t>(i);
  if (kind == tsnx::kFlush) v = t->store().written();
  g_core->commands[g_core->n_commands++] = {kind, id, v, d};
  return 0;
}

EMSCRIPTEN_KEEPALIVE int32_t tsnxw_dispose(int32_t id) {
  const int s = g_core->Slot(id);
  if (s < 0) return tsnx::kErrNoTrack;
  delete g_core->tracks[s];
  g_core->tracks[s] = nullptr;
  return 0;
}

EMSCRIPTEN_KEEPALIVE int32_t tsnxw_deliver(int32_t id, double index,
                                           const int16_t* pcm, double frames) {
  const int s = g_core->Slot(id);
  if (s < 0) return tsnx::kErrNoTrack;
  g_core->tracks[s]->store().Deliver(static_cast<int64_t>(index), pcm,
                                     static_cast<int64_t>(frames));
  return 0;
}

EMSCRIPTEN_KEEPALIVE int32_t tsnxw_fail(int32_t id) {
  const int s = g_core->Slot(id);
  if (s < 0) return tsnx::kErrNoTrack;
  g_core->tracks[s]->store().Fail();
  return 0;
}

// The browser's output rate (a multiple of 100). Blocks are rate / 100
// frames. Also the capture input rate.
EMSCRIPTEN_KEEPALIVE int32_t tsnxw_set_rate(int32_t rate) {
  if (rate < 8000 || rate > 192000 || rate % 100 != 0)
    return tsnx::kErrUnsupportedFormat;
  g_core->SetOutputRate(rate);
  return 0;
}

// Renders one 10 ms block. Returns the planar stereo output at the rate
// from tsnxw_set_rate (rate / 100 left, then right, float -1..1). `mix_ns`: when the block's first
// frame is rendered; `now_ns`: now; both on the render clock.
EMSCRIPTEN_KEEPALIVE float* tsnxw_render(double mix_ns, double delay_ns,
                                         double now_ns) {
  g_core->Render(static_cast<int64_t>(mix_ns), static_cast<int64_t>(delay_ns),
                 static_cast<int64_t>(now_ns));
  return g_core->out.data();
}

// The output stopped at `stop_ns` and will start again; the audio in the
// device buffer is lost (ADR I10).
EMSCRIPTEN_KEEPALIVE void tsnxw_output_restart(double stop_ns) {
  for (tsnx::Track* t : g_core->tracks)
    if (t) t->OnOutputRestart(static_cast<int64_t>(stop_ns));
}

// Applies commands and publishes state without rendering (no output).
EMSCRIPTEN_KEEPALIVE void tsnxw_idle(double now_ns) {
  g_core->retirer.BeginBlock();
  g_core->ApplyCommands();
  g_core->Publish(static_cast<int64_t>(now_ns));
  g_core->retirer.EndBlock();
  g_core->Maintain();
}

// Results of the last calls. The host reads them, then calls tsnxw_clear.
EMSCRIPTEN_KEEPALIVE double* tsnxw_states() { return g_core->states; }
EMSCRIPTEN_KEEPALIVE int32_t tsnxw_state_count() { return g_core->n_states; }
EMSCRIPTEN_KEEPALIVE double* tsnxw_events() { return g_core->events; }
EMSCRIPTEN_KEEPALIVE int32_t tsnxw_event_count() { return g_core->n_events; }
EMSCRIPTEN_KEEPALIVE double* tsnxw_requests() { return g_core->requests; }
EMSCRIPTEN_KEEPALIVE int32_t tsnxw_request_count() {
  return g_core->n_requests;
}
EMSCRIPTEN_KEEPALIVE void tsnxw_clear() {
  g_core->n_events = 0;
  g_core->n_requests = 0;
}

// Capture: the host fills tsnxw_capture_in() with rate / 100 mono samples
// at the tsnxw_set_rate rate (float, -1..1), then calls
// tsnxw_capture(out_rate). Returns PCM16 at `out_rate`: out_rate / 100
// samples.
EMSCRIPTEN_KEEPALIVE float* tsnxw_capture_in() { return g_core->cap_in.data(); }
EMSCRIPTEN_KEEPALIVE int16_t* tsnxw_capture(int32_t out_rate) {
  tsnx::Core& c = *g_core;
  if (out_rate < 8000 || out_rate > 48000 || out_rate % 100 != 0)
    return nullptr;
  const int in_frames = c.out_frames;
  const int frames = out_rate / 100;
  c.cap_f.resize(static_cast<size_t>(in_frames));
  c.cap_r.resize(static_cast<size_t>(frames));
  c.cap_out.resize(static_cast<size_t>(frames));
  for (int i = 0; i < in_frames; ++i) c.cap_f[i] = c.cap_in[i] * 32768.0f;
  const float* src = c.cap_f.data();
  if (c.out_rate != out_rate) {
    if (!c.cap_rs || c.cap_from != c.out_rate || c.cap_to != out_rate) {
      c.cap_rs = std::make_unique<tsnx::BlockResampler>(c.out_rate, out_rate, 1);
      c.cap_from = c.out_rate;
      c.cap_to = out_rate;
    }
    c.cap_rs->Process(c.cap_f.data(), c.cap_r.data());
    src = c.cap_r.data();
  }
  for (int i = 0; i < frames; ++i) {
    const float v = std::clamp(src[i], -32768.0f, 32767.0f);
    c.cap_out[i] = static_cast<int16_t>(std::lrintf(v));
  }
  return c.cap_out.data();
}

// ---- MP3 (storage worker) ----
// Takes ownership of `data` (from tsnxw_alloc). Returns a handle or 0.
EMSCRIPTEN_KEEPALIVE void* tsnxw_mp3_open(uint8_t* data, int32_t size) {
  auto* f = new tsnx::Mp3File;
  f->bytes.assign(data, data + size);
  std::free(data);
  std::memset(&f->dec, 0, sizeof(f->dec));
  if (mp3dec_ex_open_buf(&f->dec, f->bytes.data(), f->bytes.size(),
                         MP3D_SEEK_TO_SAMPLE) != 0 ||
      f->dec.samples == 0 || f->dec.info.channels < 1 ||
      f->dec.info.channels > 2 || f->dec.info.hz <= 0 ||
      f->dec.info.hz % 100 != 0) {
    mp3dec_ex_close(&f->dec);
    delete f;
    return nullptr;
  }
  return f;
}
EMSCRIPTEN_KEEPALIVE int32_t tsnxw_mp3_rate(tsnx::Mp3File* f) {
  return f->dec.info.hz;
}
EMSCRIPTEN_KEEPALIVE int32_t tsnxw_mp3_channels(tsnx::Mp3File* f) {
  return f->dec.info.channels;
}
EMSCRIPTEN_KEEPALIVE double tsnxw_mp3_frames(tsnx::Mp3File* f) {
  return static_cast<double>(f->dec.samples / f->dec.info.channels);
}
// Decodes `frames` frames from `frame` into `out` (zero-filled past the
// end, as the native store does). Returns 0, or -1 if the seek failed.
EMSCRIPTEN_KEEPALIVE int32_t tsnxw_mp3_read(tsnx::Mp3File* f, double frame,
                                            int32_t frames, int16_t* out) {
  const int ch = f->dec.info.channels;
  if (mp3dec_ex_seek(&f->dec, static_cast<uint64_t>(frame) * ch) != 0)
    return -1;
  const size_t want = static_cast<size_t>(frames) * ch;
  const size_t got = mp3dec_ex_read(&f->dec, out, want);
  if (got < want) std::memset(out + got, 0, (want - got) * sizeof(int16_t));
  return 0;
}
EMSCRIPTEN_KEEPALIVE void tsnxw_mp3_close(tsnx::Mp3File* f) {
  mp3dec_ex_close(&f->dec);
  delete f;
}

}  // extern "C"
