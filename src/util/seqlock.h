// Seqlock for a small struct written by one thread (the audio thread) and
// read by others. The writer never waits. Fields are copied with relaxed
// atomic word accesses, so there is no data race in the C++ sense.
#ifndef TSNX_UTIL_SEQLOCK_H_
#define TSNX_UTIL_SEQLOCK_H_

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace tsnx {

template <typename T>
class Seqlock {
  static_assert(std::is_trivially_copyable_v<T>);
  static_assert(sizeof(T) % sizeof(uint64_t) == 0);
  static constexpr size_t kWords = sizeof(T) / sizeof(uint64_t);

 public:
  Seqlock() {
    T zero{};
    Store(zero);
  }

  void Store(const T& value) {
    uint64_t words[kWords];
    std::memcpy(words, &value, sizeof(T));
    const uint32_t s = seq_.load(std::memory_order_relaxed);
    seq_.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (size_t i = 0; i < kWords; ++i)
      words_[i].store(words[i], std::memory_order_relaxed);
    seq_.store(s + 2, std::memory_order_release);
  }

  T Load() const {
    uint64_t words[kWords];
    for (;;) {
      const uint32_t s0 = seq_.load(std::memory_order_acquire);
      if (s0 & 1) continue;
      for (size_t i = 0; i < kWords; ++i)
        words[i] = words_[i].load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (seq_.load(std::memory_order_relaxed) == s0) break;
    }
    T value;
    std::memcpy(&value, words, sizeof(T));
    return value;
  }

 private:
  std::atomic<uint32_t> seq_{0};
  std::atomic<uint64_t> words_[kWords];
};

}  // namespace tsnx

#endif  // TSNX_UTIL_SEQLOCK_H_
