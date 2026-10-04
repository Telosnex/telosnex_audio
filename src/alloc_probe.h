// Counts heap allocations made on a thread marked as real-time. Used by the
// I1 test. Without TSNX_ALLOC_PROBE the hooks are plain calloc/realloc and
// the marker is a no-op.
#ifndef TSNX_ALLOC_PROBE_H_
#define TSNX_ALLOC_PROBE_H_

#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#if TSNX_ALLOC_PROBE
void* tsnx_probe_calloc(size_t n, size_t s);
void* tsnx_probe_realloc(void* p, size_t s);
void* tsnx_probe_malloc(size_t s);
void tsnx_probe_note_alloc(void);
int tsnx_probe_rt_enter(void);
void tsnx_probe_rt_exit(int previous);
long tsnx_probe_rt_allocs(void);
#else
static inline void* tsnx_probe_calloc(size_t n, size_t s) { return calloc(n, s); }
static inline void* tsnx_probe_realloc(void* p, size_t s) { return realloc(p, s); }
static inline void* tsnx_probe_malloc(size_t s) { return malloc(s); }
static inline int tsnx_probe_rt_enter(void) { return 0; }
static inline void tsnx_probe_rt_exit(int previous) { (void)previous; }
#endif

#ifdef __cplusplus
}

namespace tsnx {
// Marks the current scope as audio-thread code for the allocation probe.
class RtScope {
 public:
  RtScope() : previous_(tsnx_probe_rt_enter()) {}
  ~RtScope() { tsnx_probe_rt_exit(previous_); }
  RtScope(const RtScope&) = delete;
  RtScope& operator=(const RtScope&) = delete;

 private:
  int previous_;
};
}  // namespace tsnx
#endif

#endif  // TSNX_ALLOC_PROBE_H_
