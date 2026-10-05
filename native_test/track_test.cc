// Track behavior through the engine with the manual device (ADR I4-I7, I11).
#include <filesystem>
#include <random>

#include "harness.h"

using namespace tsnx_test;

namespace {
constexpr int kAll = 0;
constexpr int kUnplayed = 1;
constexpr double kF0 = 300, kK = 150;  // chirp: f(t) = f0 + k t

std::vector<int16_t> Chirp(int rate, double secs) {
  std::vector<int16_t> v(static_cast<size_t>(rate * secs));
  for (size_t i = 0; i < v.size(); ++i) {
    const double t = static_cast<double>(i) / rate;
    v[i] = static_cast<int16_t>(
        12000 * std::sin(2 * M_PI * (kF0 * t + 0.5 * kK * t * t)));
  }
  return v;
}

// Median per-period frequency from interpolated rising zero crossings.
double LocalFreq(const int16_t* x, int n, int rate) {
  std::vector<double> zc;
  for (int i = 1; i < n; ++i)
    if (x[i - 1] < 0 && x[i] >= 0)
      zc.push_back(i - 1 + static_cast<double>(-x[i - 1]) / (x[i] - x[i - 1]));
  std::vector<double> f;
  for (size_t i = 1; i < zc.size(); ++i) f.push_back(rate / (zc[i] - zc[i - 1]));
  if (f.size() < 3) return NAN;
  std::sort(f.begin(), f.end());
  return f[f.size() / 2];
}

void WriteAll(Harness& h, int32_t id, const std::vector<int16_t>& pcm,
              int channels) {
  CHECK_EQ(tsnx_track_write(h.e, id, pcm.data(),
                            static_cast<int64_t>(pcm.size() / channels)),
           static_cast<int64_t>(pcm.size() / channels));
}
}  // namespace

// I4: heard position vs chirp ground truth, through rate changes and a seek.
TEST(TrackPositionFollowsChirp) {
  Harness h;
  const int rate = 24000;
  const int32_t id = h.NewTrack(rate, 1, kAll);
  WriteAll(h, id, Chirp(rate, 16), 1);
  tsnx_track_end_of_stream(h.e, id);
  tsnx_track_play(h.e, id);
  struct Ev { int block; double speed; int64_t seek; };
  const std::vector<Ev> evs = {{200, 1.5, -1}, {500, 2.0, -1}, {750, 1.0, -1},
                               {850, 1.5, -1}, {950, 0, 3 * rate},
                               {1150, 2.0, -1}};
  std::vector<int16_t> out;
  std::vector<double> pos_at_start;  // heard position at each block start
  size_t ei = 0;
  std::vector<int> seek_blocks;
  int64_t last = -1;
  bool monotonic = true, bounded = true;
  for (int b = 0; b < 1400; ++b) {
    if (ei < evs.size() && evs[ei].block == b) {
      if (evs[ei].seek >= 0) {
        tsnx_track_seek(h.e, id, evs[ei].seek);
        seek_blocks.push_back(b);
        last = -1;
      } else {
        tsnx_track_set_rate(h.e, id, evs[ei].speed);
      }
      ++ei;
    }
    h.Render(1, &out);
    const auto s = h.State(id);
    pos_at_start.push_back(static_cast<double>(s.position) / rate);
    if (s.status == 4) break;
    const bool after_seek =
        !seek_blocks.empty() && b - seek_blocks.back() <= 1;
    if (last >= 0 && s.position < last && !after_seek) {
      if (monotonic)
        std::fprintf(stderr, "  decrease at block %d: %lld -> %lld\n", b,
                     (long long)last, (long long)s.position);
      monotonic = false;
    }
    if (s.position > s.written) bounded = false;
    last = s.position;
  }
  CHECK(monotonic);
  CHECK(bounded);
  std::vector<double> err;
  for (size_t b = 1; b + 1 < pos_at_start.size(); ++b) {
    bool near_seek = false;
    for (int sb : seek_blocks)
      if (std::abs(static_cast<int>(b) - sb) < 4) near_seek = true;
    if (near_seek) continue;
    const double f = LocalFreq(out.data() + b * 480 - 240, 960, 48000);
    const double truth = (f - kF0) / kK;
    if (std::isnan(f) || truth < 0.05 || truth > 15.9) continue;
    const double est = (pos_at_start[b] + pos_at_start[b + 1]) / 2;
    err.push_back(std::abs(est - truth) * 1000);
  }
  std::fprintf(stderr, "  position error ms: p50 %.1f p95 %.1f max %.1f (n=%zu)\n",
               Pct(err, .5), Pct(err, .95), Pct(err, 1), err.size());
  CHECK(err.size() > 800);
  CHECK_MSG(Pct(err, .95) <= 15.0, "p95 %.1f ms", Pct(err, .95));
}

// I4: after ended, position equals duration.
TEST(TrackEndedPositionIsDuration) {
  Harness h;
  const int32_t id = h.NewTrack(24000, 1, kAll);
  WriteAll(h, id, Sine(24000, 1, 440, 0.5), 1);
  tsnx_track_end_of_stream(h.e, id);
  tsnx_track_play(h.e, id);
  h.Render(80);
  const auto s = h.State(id);
  CHECK_EQ(s.status, 4);
  CHECK_EQ(s.position, 12000);
  CHECK_EQ(s.duration, 12000);
}

// I5: pause, resume, seek, and flush ramp; no earlier audio after a cut.
TEST(TrackCutsRampAndLeaveNoOldAudio) {
  Harness h;
  const int rate = 48000;  // passthrough: no resampler between Sonic and out
  const int32_t id = h.NewTrack(rate, 1, kAll);
  auto pcm = Sine(rate, 1, 1000, 2.0);
  pcm.resize(static_cast<size_t>(rate * 4), 0);  // 2 s tone, 2 s silence
  WriteAll(h, id, pcm, 1);
  tsnx_track_end_of_stream(h.e, id);
  const int tone_step = static_cast<int>(2 * M_PI * 1000 / rate * 16000) + 2;

  std::vector<int16_t> out;
  tsnx_track_play(h.e, id);
  h.Render(20, &out);
  CHECK_MSG(MaxStep(out.data(), out.size()) <= tone_step, "start step %d",
            MaxStep(out.data(), out.size()));
  out.clear();
  tsnx_track_pause(h.e, id);
  h.Render(5, &out);
  CHECK_MSG(MaxStep(out.data(), out.size()) <= tone_step, "pause step %d",
            MaxStep(out.data(), out.size()));
  bool silent = true;
  for (size_t i = 480; i < out.size(); ++i) silent &= out[i] == 0;
  CHECK_MSG(silent, "audio after pause ramp");
  CHECK_EQ(h.State(id).status, 2);

  out.clear();
  tsnx_track_play(h.e, id);
  h.Render(5, &out);
  CHECK_MSG(MaxStep(out.data(), out.size()) <= tone_step, "resume step %d",
            MaxStep(out.data(), out.size()));

  // Seek into the silent half while the tone plays.
  out.clear();
  tsnx_track_seek(h.e, id, 3 * rate);
  h.Render(10, &out);
  CHECK_MSG(MaxStep(out.data(), out.size()) <= tone_step, "seek step %d",
            MaxStep(out.data(), out.size()));
  int after_cut = 0;
  for (size_t i = 480; i < out.size(); ++i) after_cut = std::max(after_cut, std::abs(out[i]));
  CHECK_MSG(after_cut == 0, "old audio after seek: %d", after_cut);
  const auto s = h.State(id);
  CHECK(s.position >= 3 * rate && s.position < 3 * rate + rate / 5);

  // Flush on a live track: no written-before-flush audio plays after it.
  const int32_t live = h.NewTrack(rate, 1, kUnplayed);
  WriteAll(h, live, Sine(rate, 1, 1000, 1.0), 1);
  tsnx_track_play(h.e, live);
  h.Render(10);
  out.clear();
  tsnx_track_flush(h.e, live);
  h.Render(10, &out);
  int live_after = 0;
  for (size_t i = 480; i < out.size(); ++i) live_after = std::max(live_after, std::abs(out[i]));
  CHECK_MSG(live_after == 0, "old audio after flush: %d", live_after);
  CHECK_MSG(MaxStep(out.data(), out.size()) <= tone_step, "flush step %d",
            MaxStep(out.data(), out.size()));
  CHECK_EQ(h.Count(TSNX_NOTIFY_FLUSHED, live), 1);
  int64_t cut = -1;
  for (const auto& ev : Events())
    if (ev.kind == TSNX_NOTIFY_FLUSHED && ev.id == live) cut = ev.value;
  // 10 blocks played plus the 5 ms fade-out.
  CHECK_MSG(cut >= 10 * 480 && cut <= 11 * 480, "cut %lld", (long long)cut);
  // New audio after the flush plays.
  WriteAll(h, live, Sine(rate, 1, 500, 0.2), 1);
  out.clear();
  h.Render(5, &out);
  int fresh = 0;
  for (int16_t v : out) fresh = std::max(fresh, std::abs(v));
  CHECK(fresh > 10000);
}

// I5: Sonic at rate 1.0 copies samples through.
TEST(TrackRateOneIsBitExact) {
  Harness h;
  const int32_t id = h.NewTrack(48000, 1, kAll);
  std::vector<int16_t> pcm(48000);
  std::mt19937 rng(3);
  for (auto& v : pcm) v = static_cast<int16_t>(rng() % 20000 - 10000);
  WriteAll(h, id, pcm, 1);
  tsnx_track_play(h.e, id);
  std::vector<int16_t> out;
  h.Render(50, &out);
  // Skip the 5 ms fade-in.
  bool exact = true;
  for (size_t i = 240; i < 24000; ++i) exact &= out[i] == pcm[i];
  CHECK(exact);
}

// I6: paused and starved tracks are silent and hold their position.
TEST(TrackPausedAndStarvedHoldPosition) {
  Harness h;
  const int32_t id = h.NewTrack(24000, 1, kUnplayed);
  WriteAll(h, id, Sine(24000, 1, 440, 0.3), 1);
  tsnx_track_play(h.e, id);
  h.Render(40);
  CHECK_EQ(h.State(id).status, 3);
  CHECK_EQ(h.Count(TSNX_NOTIFY_STARVED, id), 1);
  const int64_t p0 = h.State(id).position;
  std::vector<int16_t> out;
  h.Render(10, &out);
  CHECK_EQ(h.State(id).position, p0);
  CHECK_EQ(p0, 7200);
  int peak = 0;
  for (int16_t v : out) peak = std::max(peak, std::abs(v));
  CHECK_EQ(peak, 0);
  // More audio resumes it.
  WriteAll(h, id, Sine(24000, 1, 440, 0.3), 1);
  h.Render(5);
  CHECK_EQ(h.State(id).status, 1);
  CHECK_EQ(h.Count(TSNX_NOTIFY_RESUMED, id), 1);
  tsnx_track_pause(h.e, id);
  h.Render(2);
  const int64_t p1 = h.State(id).position;
  out.clear();
  h.Render(10, &out);
  CHECK_EQ(h.State(id).position, p1);
  CHECK_EQ(h.State(id).status, 2);
  peak = 0;
  for (int16_t v : out) peak = std::max(peak, std::abs(v));
  CHECK_EQ(peak, 0);
}

// I7: one `ended`, after endOfStream and after the device delay.
TEST(TrackEndedOnceAfterDeviceDelay) {
  Harness h(/*delay_ms=*/100);
  const int32_t id = h.NewTrack(48000, 1, kAll);
  WriteAll(h, id, Sine(48000, 1, 440, 0.2), 1);
  tsnx_track_play(h.e, id);
  std::vector<int16_t> out;
  h.Render(40, &out);  // no endOfStream yet: starved, not ended
  CHECK_EQ(h.Count(TSNX_NOTIFY_ENDED, id), 0);
  CHECK_EQ(h.State(id).status, 3);
  tsnx_track_end_of_stream(h.e, id);
  out.clear();
  const int64_t base = BlockCounter();
  h.Render(30, &out);
  CHECK_EQ(h.Count(TSNX_NOTIFY_ENDED, id), 1);
  int64_t ended_block = -1;
  for (const auto& ev : Events())
    if (ev.kind == TSNX_NOTIFY_ENDED) ended_block = ev.block - base;
  // The last frame was rendered at the first block after endOfStream; it is
  // heard 100 ms (10 blocks) later.
  CHECK_MSG(ended_block >= 10 && ended_block <= 12, "ended at block %lld",
            (long long)ended_block);
  h.Render(20);
  CHECK_EQ(h.Count(TSNX_NOTIFY_ENDED, id), 1);
  // `started` once, at the heard time of the first audio.
  CHECK_EQ(h.Count(TSNX_NOTIFY_STARTED, id), 1);
  int64_t started_block = -1;
  for (const auto& ev : Events())
    if (ev.kind == TSNX_NOTIFY_STARTED) started_block = ev.block;
  CHECK_MSG(started_block >= 10 && started_block <= 11, "started at %lld",
            (long long)started_block);
}

// I11: a far seek in a spilled PCM track plays the written samples.
TEST(TrackFarSeekInSpilledTrackIsExact) {
  const std::string dir = TempDir("farseek");
  {
    Harness h(0, 1, dir.c_str());
    const int rate = 48000;
    const int32_t id = h.NewTrack(rate, 1, kAll);
    std::vector<int16_t> sec(rate);
    for (int s = 0; s < 100; ++s) {
      for (int i = 0; i < rate; ++i)
        sec[i] = static_cast<int16_t>(((s * rate + i) * 13) % 25000 - 12500);
      WriteAll(h, id, sec, 1);
      h.Render(1);  // control passes spill as the track grows
    }
    tsnx_track_end_of_stream(h.e, id);
    bool spill_exists = false;
    for (auto& p : std::filesystem::recursive_directory_iterator(dir))
      if (p.path().extension() == ".pcm") spill_exists = true;
    CHECK(spill_exists);
    const int64_t target = 80 * rate + 12345;
    tsnx_track_seek(h.e, id, target);
    h.Render(1);  // applies the seek; the next control pass loads the window
    tsnx_track_play(h.e, id);
    std::vector<int16_t> out;
    h.Render(10, &out);
    bool exact = true;
    for (size_t i = 240; i < out.size(); ++i)
      exact &= out[i] == static_cast<int16_t>(
                             ((target + static_cast<int64_t>(i)) * 13) % 25000 -
                             12500);
    CHECK(exact);
    tsnx_track_dispose(h.e, id);
    h.Render(1);
    bool left = false;
    for (auto& p : std::filesystem::recursive_directory_iterator(dir))
      if (p.path().extension() == ".pcm") left = true;
    CHECK_MSG(!left, "spill file left after dispose");
  }
  std::filesystem::remove_all(dir);
}

// I11: spill folders of dead processes are deleted at engine open.
TEST(EngineDeletesStaleSpillAtOpen) {
  const std::string dir = TempDir("stale");
  std::filesystem::create_directories(dir + "/999999");
  FILE* f = std::fopen((dir + "/999999/track_8.pcm").c_str(), "wb");
  if (f) std::fclose(f);
  {
    Harness h(0, 1, dir.c_str());
    CHECK(!std::filesystem::exists(dir + "/999999"));
  }
  std::filesystem::remove_all(dir);
}

TEST(Mp3TrackPlaysToEnd) {
  std::vector<uint8_t> bytes;
  {
    FILE* f = std::fopen(TSNX_FIXTURES "/shine_48k_stereo_128k.mp3", "rb");
    CHECK(f != nullptr);
    if (!f) return;
    std::fseek(f, 0, SEEK_END);
    bytes.resize(static_cast<size_t>(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    CHECK_EQ(std::fread(bytes.data(), 1, bytes.size(), f), bytes.size());
    std::fclose(f);
  }
  Harness h(0, 2);
  int32_t id = -1;
  CHECK_EQ(tsnx_track_create_mp3(h.e, bytes.data(),
                                 static_cast<int64_t>(bytes.size()), &id),
           0);
  int32_t rate = 0, ch = 0;
  tsnx_track_format(h.e, id, &rate, &ch);
  CHECK_EQ(rate, 48000);
  CHECK_EQ(ch, 2);
  const auto s0 = h.State(id);
  CHECK(s0.duration >= 8 * 48000 && s0.duration < 9 * 48000);
  tsnx_track_set_rate(h.e, id, 3.0);
  tsnx_track_play(h.e, id);
  std::vector<int16_t> out;
  h.Render(320, &out);
  int peak = 0;
  for (int16_t v : out) peak = std::max(peak, std::abs(v));
  CHECK(peak > 1000);
  CHECK_EQ(h.State(id).status, 4);
  CHECK_EQ(h.Count(TSNX_NOTIFY_ENDED, id), 1);
}

// I10: after an output restart, playback continues from the heard position.
// The device buffer (40 ms here) is lost, so the engine moves the track back
// by that much: no audio is skipped, and none plays twice.
TEST(OutputRestartResumesFromHeardPosition) {
  for (int retention : {kAll, kUnplayed}) {
    Harness h(/*delay_ms=*/40);
    const int rate = 48000;
    // Source sample i holds i % 30000, so an output sample names its frame.
    std::vector<int16_t> src(static_cast<size_t>(rate * 3));
    for (size_t i = 0; i < src.size(); ++i)
      src[i] = static_cast<int16_t>(i % 30000);
    const int32_t id = h.NewTrack(rate, 1, retention);
    WriteAll(h, id, src, 1);
    tsnx_track_end_of_stream(h.e, id);
    tsnx_track_play(h.e, id);
    // 104 blocks: the restart falls 0.04 s past the 1 s chunk boundary, so
    // .unplayed has already moved its playhead into the next chunk.
    std::vector<int16_t> before;
    h.Render(104, &before);
    // 1.04 s rendered, 40 ms still in the device: 1.00 s heard when the
    // output stops. (The published state is from the last block's start.)
    const int64_t heard_at_stop = 48000;
    CHECK_MSG(std::abs(h.State(id).position - (heard_at_stop - 480)) <= 48,
              "state %lld", static_cast<long long>(h.State(id).position));

    CHECK_EQ(tsnx_engine_manual_output_restart(h.e, 300), 0);
    std::vector<int16_t> after;
    h.Render(3, &after);
    // Past the 5 ms fade-in, output sample k is source frame heard_at_stop+k.
    const int k = 480;
    const int64_t frame = after[k];
    const int64_t want = (heard_at_stop + k) % 30000;
    CHECK_MSG(std::abs(frame - want) <= 48, "resumed at %lld, want %lld",
              static_cast<long long>(frame), static_cast<long long>(want));
    CHECK(h.State(id).status == 1);
  }
}
