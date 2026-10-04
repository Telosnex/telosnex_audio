#include "alloc_probe.h"

#if TSNX_ALLOC_PROBE
#include <atomic>

namespace {
thread_local int g_rt = 0;
std::atomic<long> g_rt_allocs{0};
}  // namespace

extern "C" {
void tsnx_probe_note_alloc(void) {
  if (g_rt) g_rt_allocs.fetch_add(1, std::memory_order_relaxed);
}
void* tsnx_probe_calloc(size_t n, size_t s) {
  tsnx_probe_note_alloc();
  return calloc(n, s);
}
void* tsnx_probe_realloc(void* p, size_t s) {
  tsnx_probe_note_alloc();
  return realloc(p, s);
}
void* tsnx_probe_malloc(size_t s) {
  tsnx_probe_note_alloc();
  return malloc(s);
}
int tsnx_probe_rt_enter(void) {
  const int previous = g_rt;
  g_rt = 1;
  return previous;
}
void tsnx_probe_rt_exit(int previous) { g_rt = previous; }
long tsnx_probe_rt_allocs(void) { return g_rt_allocs.load(); }
}
#endif
