// Test helpers: a manual-device engine driven through the C API.
#ifndef TSNX_NATIVE_TEST_HARNESS_H_
#define TSNX_NATIVE_TEST_HARNESS_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <string>
#include <vector>

#include "test.h"
#include "tsnx_audio.h"

namespace tsnx_test {

struct Event {
  int32_t kind;
  int32_t id;
  int64_t value;
  int64_t block;  // render block count when delivered
};

inline std::vector<Event>& Events() {
  static std::vector<Event> events;
  return events;
}
inline int64_t& BlockCounter() {
  static int64_t n = 0;
  return n;
}
inline void OnNotify(int32_t kind, int32_t id, int64_t value) {
  Events().push_back({kind, id, value, BlockCounter()});
}

struct Harness {
  explicit Harness(int delay_ms = 0, int channels = 1,
                   const char* spill_dir = nullptr, bool aec = true,
                   bool ns = false, bool agc = false, int clock = 0)
      : channels(channels) {
    Events().clear();
    BlockCounter() = 0;
    tsnx_engine_config c{};
    c.manual_device = 1;
    c.manual_output_channels = channels;
    c.manual_delay_ms = delay_ms;
    c.echo_cancellation = aec;
    c.noise_suppression = ns;
    c.auto_gain = agc;
    c.spill_dir = spill_dir;
    c.clock_correction = clock;
    c.notify = &OnNotify;
    CHECK_EQ(tsnx_engine_open(&c, &e), 0);
  }
  ~Harness() { tsnx_engine_close(e); }

  int32_t NewTrack(int rate, int ch, int retention) {
    int32_t id = -1;
    CHECK_EQ(tsnx_track_create(e, rate, ch, retention, &id), 0);
    return id;
  }

  // Renders blocks and appends to `out` (48 kHz, `channels` interleaved).
  void Render(int blocks, std::vector<int16_t>* out = nullptr,
              const int16_t* capture = nullptr) {
    std::vector<int16_t> buf(static_cast<size_t>(blocks) * 480 * channels);
    for (int b = 0; b < blocks; ++b) {
      CHECK_EQ(tsnx_engine_manual_render(
                   e, 1, buf.data() + static_cast<size_t>(b) * 480 * channels,
                   capture ? capture + static_cast<size_t>(b) * 480 : nullptr),
               0);
      ++BlockCounter();
    }
    if (out) out->insert(out->end(), buf.begin(), buf.end());
  }

  tsnx_track_state State(int32_t id) {
    tsnx_track_state s{};
    CHECK_EQ(tsnx_track_get_state(e, id, &s), 0);
    return s;
  }

  int Count(int32_t kind, int32_t id) const {
    int n = 0;
    for (const auto& ev : Events())
      if (ev.kind == kind && ev.id == id) ++n;
    return n;
  }

  tsnx_engine* e = nullptr;
  int channels;
};

inline std::vector<int16_t> Sine(int rate, int channels, double hz,
                                 double seconds, double amp = 16000) {
  const size_t n = static_cast<size_t>(rate * seconds);
  std::vector<int16_t> v(n * channels);
  for (size_t i = 0; i < n; ++i) {
    const auto s = static_cast<int16_t>(
        amp * std::sin(2 * M_PI * hz * static_cast<double>(i) / rate));
    for (int c = 0; c < channels; ++c) v[i * channels + c] = s;
  }
  return v;
}

inline double Pct(std::vector<double> v, double p) {
  if (v.empty()) return NAN;
  std::sort(v.begin(), v.end());
  return v[static_cast<size_t>(p * (v.size() - 1))];
}

inline int MaxStep(const int16_t* x, size_t n, size_t stride = 1) {
  int m = 0;
  for (size_t i = stride; i < n; i += stride)
    m = std::max(m, std::abs(x[i] - x[i - stride]));
  return m;
}

inline std::string TempDir(const char* name) {
  const char* t = std::getenv("TMPDIR");
  std::string d = std::string(t ? t : "/tmp") + "/tsnx_test_" + name + "_" +
                  std::to_string(::getpid());
  return d;
}

}  // namespace tsnx_test

#endif  // TSNX_NATIVE_TEST_HARNESS_H_
