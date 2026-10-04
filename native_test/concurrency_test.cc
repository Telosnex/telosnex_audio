// Render on one thread while another writes, sends commands, reads state,
// and creates and disposes tracks. Run under ThreadSanitizer (ADR I1 note).
#include <atomic>
#include <filesystem>
#include <thread>

#include "harness.h"

using namespace tsnx_test;

TEST(ConcurrentRenderAndControl) {
  const std::string dir = TempDir("conc");
  tsnx_engine_config c{};
  c.manual_device = 1;
  c.manual_output_channels = 2;
  c.echo_cancellation = 1;
  c.spill_dir = dir.c_str();
  tsnx_engine* e = nullptr;
  CHECK_EQ(tsnx_engine_open(&c, &e), 0);
  std::atomic<bool> stop{false};
  std::thread render([&] {
    std::vector<int16_t> out(480 * 2);
    std::vector<int16_t> cap(480, 100);
    while (!stop.load()) tsnx_engine_manual_render(e, 1, out.data(), cap.data());
  });
  tsnx_capture_start(e, 24000, 1);
  const auto chunk = Sine(24000, 1, 330, 0.1);
  tsnx_capture_block blocks[32];
  for (int round = 0; round < 40; ++round) {
    int32_t live = -1, saved = -1;
    tsnx_track_create(e, 24000, 1, 1, &live);
    tsnx_track_create(e, 24000, 1, 0, &saved);
    tsnx_track_play(e, live);
    tsnx_track_play(e, saved);
    for (int i = 0; i < 50; ++i) {
      tsnx_track_write(e, live, chunk.data(), 2400);
      tsnx_track_write(e, saved, chunk.data(), 2400);
      tsnx_track_state s{};
      tsnx_track_get_state(e, saved, &s);
      if (i % 7 == 0) tsnx_track_seek(e, saved, s.position / 2);
      if (i % 11 == 0) tsnx_track_set_rate(e, saved, 1.0 + (i % 3) * 0.5);
      if (i % 13 == 0) tsnx_track_flush(e, live);
      tsnx_capture_read(e, blocks, 32);
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    tsnx_track_dispose(e, live);
    tsnx_track_dispose(e, saved);
  }
  stop.store(true);
  render.join();
  tsnx_engine_close(e);
  std::filesystem::remove_all(dir);
}
