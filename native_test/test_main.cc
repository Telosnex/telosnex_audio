#include <cstdlib>
#include <cstring>
#include <new>

#include "alloc_probe.h"
#include "test.h"

namespace tsnx_test {
std::vector<Case>& Registry() {
  static std::vector<Case> cases;
  return cases;
}
int g_failures = 0;
}  // namespace tsnx_test

// Count C++ allocations for the I1 probe.
void* operator new(size_t n) {
  tsnx_probe_note_alloc();
  if (void* p = std::malloc(n ? n : 1)) return p;
  std::abort();
}
void* operator new[](size_t n) {
  tsnx_probe_note_alloc();
  if (void* p = std::malloc(n ? n : 1)) return p;
  std::abort();
}
void* operator new(size_t n, const std::nothrow_t&) noexcept {
  tsnx_probe_note_alloc();
  return std::malloc(n ? n : 1);
}
void* operator new(size_t n, std::align_val_t a) {
  tsnx_probe_note_alloc();
  void* p = nullptr;
  if (posix_memalign(&p, static_cast<size_t>(a) < sizeof(void*) ? sizeof(void*) : static_cast<size_t>(a), n ? n : 1) != 0) std::abort();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { std::free(p); }

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0;
  for (auto& c : tsnx_test::Registry()) {
    if (filter && !std::strstr(c.name, filter)) continue;
    const int before = tsnx_test::g_failures;
    std::fprintf(stderr, "[ RUN  ] %s\n", c.name);
    c.fn();
    std::fprintf(stderr, "[ %s ] %s\n",
                 tsnx_test::g_failures == before ? " OK " : "FAIL", c.name);
    ++ran;
  }
  std::fprintf(stderr, "%d tests, %d failed checks\n", ran,
               tsnx_test::g_failures);
  return tsnx_test::g_failures == 0 ? 0 : 1;
}
