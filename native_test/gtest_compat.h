// Enough of the gtest surface to run the clock tests ported from the fork.
#ifndef TSNX_NATIVE_TEST_GTEST_COMPAT_H_
#define TSNX_NATIVE_TEST_GTEST_COMPAT_H_

#include <cmath>
#include <sstream>

#include "test.h"

namespace tsnx_test {
// Swallows `<< message` after an assertion.
struct Sink {
  template <typename T>
  Sink& operator<<(const T&) { return *this; }
};
}  // namespace tsnx_test

#undef TEST
#define TSNX_GT_CAT(a, b) a##_##b
#define TEST(suite, name)                                                  \
  static void TSNX_GT_CAT(suite, name)();                                  \
  static ::tsnx_test::Registrar TSNX_GT_CAT(reg_##suite, name)(            \
      #suite "." #name, TSNX_GT_CAT(suite, name));                         \
  static void TSNX_GT_CAT(suite, name)()

#define TSNX_GT_CHECK(cond, ret)                                           \
  if (!(cond)) {                                                           \
    std::fprintf(stderr, "  CHECK failed %s:%d: %s\n", __FILE__, __LINE__, \
                 #cond);                                                   \
    ++::tsnx_test::g_failures;                                             \
    ret;                                                                   \
  } else                                                                   \
    ::tsnx_test::Sink()

#define EXPECT_TRUE(c) TSNX_GT_CHECK((c), (void)0)
#define EXPECT_FALSE(c) TSNX_GT_CHECK(!(c), (void)0)
#define EXPECT_EQ(a, b) TSNX_GT_CHECK((a) == (b), (void)0)
#define EXPECT_NE(a, b) TSNX_GT_CHECK((a) != (b), (void)0)
#define EXPECT_LT(a, b) TSNX_GT_CHECK((a) < (b), (void)0)
#define EXPECT_LE(a, b) TSNX_GT_CHECK((a) <= (b), (void)0)
#define EXPECT_GT(a, b) TSNX_GT_CHECK((a) > (b), (void)0)
#define EXPECT_GE(a, b) TSNX_GT_CHECK((a) >= (b), (void)0)
#define EXPECT_NEAR(a, b, e) TSNX_GT_CHECK(std::abs((a) - (b)) <= (e), (void)0)
#define EXPECT_DOUBLE_EQ(a, b) \
  TSNX_GT_CHECK(std::abs((a) - (b)) <= 1e-9 * (1 + std::abs(b)), (void)0)
#define ASSERT_TRUE(c) TSNX_GT_CHECK((c), return)
#define ASSERT_FALSE(c) TSNX_GT_CHECK(!(c), return)
#define ASSERT_EQ(a, b) TSNX_GT_CHECK((a) == (b), return)
#define ASSERT_NE(a, b) TSNX_GT_CHECK((a) != (b), return)
#define ASSERT_NEAR(a, b, e) TSNX_GT_CHECK(std::abs((a) - (b)) <= (e), return)
#define SCOPED_TRACE(x) (void)(x)

#endif  // TSNX_NATIVE_TEST_GTEST_COMPAT_H_
