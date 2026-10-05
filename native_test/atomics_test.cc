// ADR I1/D10: the render path uses only lock-free atomic primitives.
// Compile-time checks also cover armeabi-v7a builds that cannot run on a
// 64-bit-only emulator. A hidden libatomic mutex would violate the contract.
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "harness.h"

static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<int32_t>::is_always_lock_free);
static_assert(std::atomic<int64_t>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<double>::is_always_lock_free);
static_assert(std::atomic<void*>::is_always_lock_free);
static_assert(std::atomic<size_t>::is_always_lock_free);

TEST(RenderAtomicPrimitivesAreLockFree) {
  std::atomic<bool> flag{false};
  std::atomic<int32_t> count{0};
  std::atomic<int64_t> time{0};
  std::atomic<uint64_t> word{0};
  std::atomic<double> energy{0};
  std::atomic<void*> pointer{nullptr};
  std::atomic<size_t> index{0};
  CHECK(flag.is_lock_free());
  CHECK(count.is_lock_free());
  CHECK(time.is_lock_free());
  CHECK(word.is_lock_free());
  CHECK(energy.is_lock_free());
  CHECK(pointer.is_lock_free());
  CHECK(index.is_lock_free());
}
