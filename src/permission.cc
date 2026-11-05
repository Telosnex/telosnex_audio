// Microphone permission off Apple (src/apple/permission.mm on Apple).
#include "permission.h"

#if !defined(__APPLE__)

#if defined(__ANDROID__)
#include "android/android_jni.h"
#endif

namespace tsnx::permission {

int32_t Status() {
#if defined(__ANDROID__)
  return android_jni::MicGranted() == 0 ? kNotDetermined : kGranted;
#else
  return kGranted;
#endif
}

// The Android prompt needs the activity: the plugin shows it.
void Request(std::function<void(int32_t)> done) { done(Status()); }

}  // namespace tsnx::permission

#endif  // !__APPLE__
