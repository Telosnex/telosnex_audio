// Microphone permission (src/permission.h, tsnx_mic_permission*).
#include "permission.h"
#include "test.h"
#include "tsnx_audio.h"
#include "util/dart_cobject.h"

namespace {

int64_t g_port = -1;
int64_t g_posted = -1;

bool FakePost(int64_t port, void* message) {
  auto* m = static_cast<tsnx::DartCObject*>(message);
  g_port = port;
  g_posted = m->type == tsnx::kDartCObjectInt64 ? m->value.as_int64 : -2;
  return true;
}

}  // namespace

TEST(MicPermissionStatusIsKnown) {
  const int32_t s = tsnx_mic_permission();
  CHECK(s == TSNX_MIC_GRANTED || s == TSNX_MIC_DENIED ||
        s == TSNX_MIC_NOT_DETERMINED);
  CHECK_EQ(s, tsnx::permission::Status());
#if !defined(__APPLE__) && !defined(__ANDROID__)
  CHECK_EQ(s, TSNX_MIC_GRANTED);
#endif
}

// Without a prompt to show, the request posts the status before returning.
TEST(MicPermissionRequestPostsTheKnownStatus) {
  CHECK_EQ(tsnx_mic_permission_request(7, nullptr), TSNX_ERR_INVALID_ARGUMENT);
  const int32_t s = tsnx_mic_permission();
  if (s == TSNX_MIC_NOT_DETERMINED) {
    std::fprintf(stderr, "  skipped: the request would show the prompt\n");
    return;
  }
  g_port = g_posted = -1;
  CHECK_EQ(tsnx_mic_permission_request(7, &FakePost), TSNX_OK);
  CHECK_EQ(g_port, 7);
  CHECK_EQ(g_posted, s);
}

// A manual device needs no microphone: capture starts whatever the status.
TEST(ManualCaptureNeedsNoMicPermission) {
  tsnx_engine_config config{};
  config.manual_device = 1;
  config.manual_output_channels = 2;
  tsnx_engine* engine = nullptr;
  CHECK_EQ(tsnx_engine_open(&config, &engine), TSNX_OK);
  CHECK_EQ(tsnx_capture_start(engine, 48000, 1), TSNX_OK);
  tsnx_engine_close(engine);
}
