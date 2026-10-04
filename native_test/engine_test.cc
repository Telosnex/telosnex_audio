// Engine-level invariants with the manual device (ADR I1, I2, I3).
#include <random>

#include "alloc_probe.h"
#include "harness.h"

using namespace tsnx_test;

namespace {
constexpr int kAll = 0;
constexpr int kUnplayed = 1;

// Speech-like noise: random noise with a slow 3 Hz envelope.
std::vector<int16_t> ModNoise(int rate, int channels, double secs,
                              uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> u(-1.f, 1.f);
  const size_t n = static_cast<size_t>(rate * secs);
  std::vector<int16_t> v(n * channels);
  for (size_t i = 0; i < n; ++i) {
    const double env = 0.6 + 0.4 * std::sin(2 * M_PI * 3 * i / rate);
    for (int c = 0; c < channels; ++c)
      v[i * channels + c] = static_cast<int16_t>(9000 * env * u(rng));
  }
  return v;
}

double Energy(const int16_t* x, size_t n) {
  double e = 0;
  for (size_t i = 0; i < n; ++i) e += static_cast<double>(x[i]) * x[i];
  return e + 1e-3;
}
}  // namespace

// I1: no heap allocation on the render path once running, across play,
// rate, gain, seek, pause, and flush.
TEST(EngineRenderPathDoesNotAllocate) {
#if TSNX_ALLOC_PROBE
  Harness h(20, 2);
  const int32_t a = h.NewTrack(24000, 1, kAll);
  const int32_t b = h.NewTrack(48000, 2, kUnplayed);
  const int32_t c = h.NewTrack(44100, 1, kAll);
  auto pa = ModNoise(24000, 1, 8, 1);
  auto pb = ModNoise(48000, 2, 8, 2);
  auto pc = ModNoise(44100, 1, 8, 3);
  tsnx_track_write(h.e, a, pa.data(), 24000 * 8);
  tsnx_track_write(h.e, b, pb.data(), 48000 * 8);
  tsnx_track_write(h.e, c, pc.data(), 44100 * 8);
  for (int32_t id : {a, b, c}) tsnx_track_play(h.e, id);
  const long at_start = tsnx_probe_rt_allocs();
  h.Render(5);  // first-block format setup is outside the guarantee
  const long before = tsnx_probe_rt_allocs();
  // The probe works: the first blocks set up the APM render format.
  std::fprintf(stderr, "  allocations in the first 5 blocks: %ld\n",
               before - at_start);
  for (int i = 0; i < 300; ++i) {
    switch (i) {
      case 20: tsnx_track_set_rate(h.e, a, 3.0); break;
      case 40: tsnx_track_set_rate(h.e, c, 0.5); break;
      case 60: tsnx_track_seek(h.e, a, 24000 * 2); break;
      case 80: tsnx_track_set_gain(h.e, b, 0.3, 50); break;
      case 100: tsnx_track_pause(h.e, c); break;
      case 120: tsnx_track_play(h.e, c); break;
      case 140: tsnx_track_flush(h.e, b); break;
      case 160: tsnx_track_set_rate(h.e, a, 1.5); break;
      case 180: tsnx_track_seek(h.e, c, 0); break;
      default: break;
    }
    h.Render(1);
  }
  const long allocs = tsnx_probe_rt_allocs() - before;
  CHECK_MSG(allocs == 0, "%ld allocations on the render path", allocs);
#else
  std::fprintf(stderr, "  skipped: build without TSNX_ALLOC_PROBE\n");
#endif
}

// I2 and I3: with tracks at 24 kHz and 48 kHz added and removed, the APM
// render format never changes, and the echo canceller keeps converging on
// the stretched, gain-scaled output.
TEST(EngineRenderFormatFixedAndEchoSuppressed) {
  Harness h(/*delay_ms=*/0, /*channels=*/1, nullptr, /*aec=*/true);
  const int32_t speech = h.NewTrack(24000, 1, kAll);
  auto pcm = ModNoise(24000, 1, 30, 9);
  tsnx_track_write(h.e, speech, pcm.data(), 24000 * 30);
  tsnx_track_set_rate(h.e, speech, 1.5);
  tsnx_track_set_gain(h.e, speech, 0.7, 0);
  tsnx_track_play(h.e, speech);

  const int delay_blocks = 6;  // 60 ms echo path
  std::vector<std::vector<int16_t>> history;
  std::vector<double> supp;  // dB per 100 ms window
  double in_e = 0, out_e = 0;
  int32_t lyria = -1;
  auto lyria_pcm = ModNoise(48000, 1, 6, 11);
  tsnx_capture_block blocks[64];
  int id = 0;
  CHECK_EQ(tsnx_capture_start(h.e, 48000, ++id), 0);
  for (int b = 0; b < 1600; ++b) {
    if (b == 800) {  // 8 s: a 48 kHz source starts
      lyria = h.NewTrack(48000, 1, kUnplayed);
      tsnx_track_write(h.e, lyria, lyria_pcm.data(), 48000 * 6);
      tsnx_track_play(h.e, lyria);
    }
    if (b == 1200) tsnx_track_dispose(h.e, lyria);
    std::vector<int16_t> cap(480, 0);
    if (static_cast<int>(history.size()) >= delay_blocks) {
      const auto& src = history[history.size() - delay_blocks];
      for (int i = 0; i < 480; ++i) cap[i] = static_cast<int16_t>(src[i] / 2);
    }
    std::vector<int16_t> out;
    h.Render(1, &out, cap.data());
    history.push_back(out);
    const int n = tsnx_capture_read(h.e, blocks, 64);
    for (int k = 0; k < n; ++k) {
      in_e += Energy(cap.data(), 480);
      out_e += Energy(blocks[k].data, 480);
    }
    if (b % 10 == 9) {
      supp.push_back(10 * std::log10(in_e / out_e));
      in_e = out_e = 0;
    }
  }
  CHECK_EQ(tsnx_engine_render_format_changes(h.e), 1);
  auto mean = [&](int from_s10, int to_s10) {
    double s = 0;
    for (int i = from_s10; i < to_s10; ++i) s += supp[i];
    return s / (to_s10 - from_s10);
  };
  const double before = mean(60, 80);   // 6-8 s
  const double during = mean(80, 120);  // 8-12 s, 48 kHz track playing
  const double after = mean(120, 160);  // 12-16 s
  double worst = 1e9;
  for (int i = 80; i < 160; ++i) worst = std::min(worst, supp[i]);
  std::fprintf(stderr,
               "  suppression dB: before %.1f, during %.1f, after %.1f, worst "
               "window %.1f\n",
               before, during, after, worst);
  CHECK(before > 20);
  CHECK(during > before - 6);
  CHECK(after > before - 6);
  CHECK(worst > 12);
}

// Clock correction (fork port) is wired into the capture path: a capture
// clock 2000 ppm slow engages the servo in control mode.
TEST(EngineClockCorrectionEngagesOnDrift) {
  Harness h(0, 1, nullptr, true, false, false, /*clock=*/2);
  CHECK_EQ(tsnx_capture_start(h.e, 48000, 1), 0);
  std::vector<int16_t> cap(480);
  std::vector<int16_t> out(480);
  tsnx_capture_block blocks[16];
  for (int b = 0; b < 6000; ++b) {
    for (int i = 0; i < 480; ++i)
      cap[i] = static_cast<int16_t>(3000 * std::sin(0.05 * (b * 480 + i)));
    // Every 500th block the capture device delivers nothing.
    CHECK_EQ(tsnx_engine_manual_render(h.e, 1, out.data(),
                                       b % 500 == 499 ? nullptr : cap.data()),
             0);
    while (tsnx_capture_read(h.e, blocks, 16) > 0) {
    }
  }
  double ppm = 0;
  int32_t engaged = 0;
  CHECK_EQ(tsnx_engine_clock_state(h.e, &ppm, &engaged), 2);
  std::fprintf(stderr, "  applied %.0f ppm, engaged %d\n", ppm, engaged);
  CHECK(engaged == 1);
  CHECK(std::abs(std::abs(ppm) - 2000) < 400);
}
