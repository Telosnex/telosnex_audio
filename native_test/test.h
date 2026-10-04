// Minimal test harness: TEST(name) { CHECK(...); }
#ifndef TSNX_NATIVE_TEST_TEST_H_
#define TSNX_NATIVE_TEST_TEST_H_

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace tsnx_test {
struct Case {
  const char* name;
  std::function<void()> fn;
};
std::vector<Case>& Registry();
extern int g_failures;
struct Registrar {
  Registrar(const char* name, std::function<void()> fn) {
    Registry().push_back({name, std::move(fn)});
  }
};
}  // namespace tsnx_test

#define TEST(name)                                                       \
  static void test_##name();                                             \
  static ::tsnx_test::Registrar registrar_##name(#name, test_##name);    \
  static void test_##name()

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::fprintf(stderr, "  CHECK failed %s:%d: %s\n", __FILE__,       \
                   __LINE__, #cond);                                     \
      ++::tsnx_test::g_failures;                                         \
    }                                                                    \
  } while (0)

#define CHECK_MSG(cond, ...)                                             \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::fprintf(stderr, "  CHECK failed %s:%d: %s: ", __FILE__,       \
                   __LINE__, #cond);                                     \
      std::fprintf(stderr, __VA_ARGS__);                                 \
      std::fprintf(stderr, "\n");                                        \
      ++::tsnx_test::g_failures;                                         \
    }                                                                    \
  } while (0)

#define CHECK_EQ(a, b) CHECK_MSG((a) == (b), "%lld != %lld", \
                                 (long long)(a), (long long)(b))

#endif  // TSNX_NATIVE_TEST_TEST_H_
