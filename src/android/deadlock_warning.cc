// WebRTC's Android build expects this (rtc_base/system/
// warn_current_thread_is_deadlocked.h). The WebRTC version prints Java stack
// traces through JNI; the engine has no JNI, so it logs a line.
#include "rtc_base/logging.h"
#include "rtc_base/system/warn_current_thread_is_deadlocked.h"

namespace webrtc {
void WarnThatTheCurrentThreadIsProbablyDeadlocked() {
  RTC_LOG(LS_WARNING) << "A thread waited a long time: probably deadlocked.";
}
}  // namespace webrtc
