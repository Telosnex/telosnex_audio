#include "util/wakeup.h"

#include <chrono>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <semaphore.h>
#include <time.h>
#endif

namespace tsnx {

#if defined(__APPLE__)
Wakeup::Wakeup() : impl_(dispatch_semaphore_create(0)) {}
Wakeup::~Wakeup() { dispatch_release(static_cast<dispatch_semaphore_t>(impl_)); }
void Wakeup::Signal() { dispatch_semaphore_signal(static_cast<dispatch_semaphore_t>(impl_)); }
bool Wakeup::Wait(int64_t timeout_ms) {
  return dispatch_semaphore_wait(
             static_cast<dispatch_semaphore_t>(impl_),
             dispatch_time(DISPATCH_TIME_NOW, timeout_ms * 1000000LL)) == 0;
}
#elif defined(_WIN32)
Wakeup::Wakeup() : impl_(CreateSemaphoreW(nullptr, 0, 0x7fffffff, nullptr)) {}
Wakeup::~Wakeup() { CloseHandle(static_cast<HANDLE>(impl_)); }
void Wakeup::Signal() { ReleaseSemaphore(static_cast<HANDLE>(impl_), 1, nullptr); }
bool Wakeup::Wait(int64_t timeout_ms) {
  return WaitForSingleObject(static_cast<HANDLE>(impl_),
                             static_cast<DWORD>(timeout_ms)) == WAIT_OBJECT_0;
}
#else
Wakeup::Wakeup() : impl_(new sem_t) { sem_init(static_cast<sem_t*>(impl_), 0, 0); }
Wakeup::~Wakeup() {
  sem_destroy(static_cast<sem_t*>(impl_));
  delete static_cast<sem_t*>(impl_);
}
void Wakeup::Signal() { sem_post(static_cast<sem_t*>(impl_)); }
bool Wakeup::Wait(int64_t timeout_ms) {
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += timeout_ms / 1000;
  ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec += 1;
    ts.tv_nsec -= 1000000000L;
  }
  while (sem_timedwait(static_cast<sem_t*>(impl_), &ts) != 0) {
    if (errno != EINTR) return false;
  }
  return true;
}
#endif

int64_t MonotonicNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace tsnx
