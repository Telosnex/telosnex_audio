// A counting wake-up signal. Signal() is safe on the audio thread: it does
// not allocate or take a lock (dispatch semaphore on Apple, POSIX semaphore
// elsewhere).
#ifndef TSNX_UTIL_WAKEUP_H_
#define TSNX_UTIL_WAKEUP_H_

#include <cstdint>

namespace tsnx {

class Wakeup {
 public:
  Wakeup();
  ~Wakeup();
  Wakeup(const Wakeup&) = delete;
  Wakeup& operator=(const Wakeup&) = delete;

  void Signal();
  // Returns true when signaled, false on timeout.
  bool Wait(int64_t timeout_ms);

 private:
  void* impl_;
};

// Monotonic clock in nanoseconds. The same clock as sampledAtNs in Dart.
int64_t MonotonicNowNs();

}  // namespace tsnx

#endif  // TSNX_UTIL_WAKEUP_H_
