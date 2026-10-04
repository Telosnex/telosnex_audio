// Real-device echo probe (ADR workplan step 3 gate).
//
// Plays speech from the MP3 fixture on the default output while capturing
// the default input with echo cancellation, and prints the echo level before
// and after the APM per second. Nobody should talk during the run.
//
//   echo_probe [seconds] [--no-aec] [--gain g]
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "tsnx_audio.h"

static std::vector<uint8_t> ReadFile(const char* path) {
  std::vector<uint8_t> v;
  FILE* f = std::fopen(path, "rb");
  if (!f) return v;
  std::fseek(f, 0, SEEK_END);
  v.resize(static_cast<size_t>(std::ftell(f)));
  std::fseek(f, 0, SEEK_SET);
  if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
  std::fclose(f);
  return v;
}

static void OnNotify(int32_t kind, int32_t id, int64_t value) {
  if (kind == TSNX_NOTIFY_REQUEST_DONE && value != 0)
    std::fprintf(stderr, "request %d failed: %lld\n", id, (long long)value);
  if (kind == TSNX_NOTIFY_ENGINE_ERROR)
    std::fprintf(stderr, "engine error %lld\n", (long long)value);
}

int main(int argc, char** argv) {
  int seconds = 12;
  bool aec = true;
  bool vp = false;
  bool ns = false;
  double gain = 0.5;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--no-aec")) aec = false;
    else if (!std::strcmp(argv[i], "--apple-vp")) vp = true;
    else if (!std::strcmp(argv[i], "--ns")) ns = true;
    else if (!std::strcmp(argv[i], "--gain") && i + 1 < argc) gain = std::atof(argv[++i]);
    else seconds = std::atoi(argv[i]);
  }
  const auto mp3 = ReadFile(TSNX_FIXTURES "/shine_24k_mono_64k.mp3");
  if (mp3.empty()) return 1;

  tsnx_engine_config c{};
  c.echo_cancellation = aec;
  c.platform_voice_processing = vp;
  c.noise_suppression = ns;  // off by default: measure echo removal only
  c.auto_gain = 0;
  c.notify = &OnNotify;
  tsnx_engine* e = nullptr;
  if (tsnx_engine_open(&c, &e) != 0) {
    std::fprintf(stderr, "engine open failed\n");
    return 1;
  }
  char id[256], name[256];
  for (int k = 0; k < 2; ++k)
    for (int i = 0; i < tsnx_device_count(e, k); ++i)
      if (tsnx_device_get(e, k, i, id, sizeof id, name, sizeof name) == 0)
        std::fprintf(stderr, "%s[%d] %s\n", k ? "in" : "out", i, name);

  tsnx_capture_start(e, 48000, 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  double pre = 0, post = 0;
  int64_t blocks = tsnx_engine_take_capture_energy(e, &pre, &post);
  const double noise_db = 10 * std::log10(pre / (blocks * 480.0) + 1e-9);
  std::fprintf(stderr, "room noise (no playback): %.1f dB\n", noise_db);

  int32_t track = -1;
  tsnx_track_create_mp3(e, mp3.data(), static_cast<int64_t>(mp3.size()), &track);
  tsnx_track_set_gain(e, track, gain, 0);
  tsnx_track_play(e, track);
  std::vector<double> supp;
  tsnx_capture_block buf[64];
  for (int s = 0; s < seconds; ++s) {
    for (int k = 0; k < 10; ++k) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      while (tsnx_capture_read(e, buf, 64) > 0) {
      }
    }
    blocks = tsnx_engine_take_capture_energy(e, &pre, &post);
    const double pre_db = 10 * std::log10(pre / (blocks * 480.0) + 1e-9);
    const double post_db = 10 * std::log10(post / (blocks * 480.0) + 1e-9);
    tsnx_track_state st{};
    tsnx_track_get_state(e, track, &st);
    std::fprintf(stderr,
                 "t=%2ds mic %.1f dB  after APM %.1f dB  removed %.1f dB  "
                 "erle %.1f  delay %d ms  pos %.2fs\n",
                 s + 1, pre_db, post_db, pre_db - post_db,
                 tsnx_engine_erle_db(e), tsnx_engine_output_delay_ms(e),
                 st.position / 24000.0);
    if (s >= 3) supp.push_back(pre_db - post_db);
    if (st.status == 4) {
      tsnx_track_seek(e, track, 0);
      tsnx_track_play(e, track);
    }
  }
  double mean = 0;
  for (double d : supp) mean += d;
  if (!supp.empty()) mean /= supp.size();
  std::fprintf(stderr, "mean echo removed after 3 s: %.1f dB\n", mean);
  tsnx_capture_stop(e, 2);
  tsnx_engine_close(e);
  return 0;
}
