// Single-producer single-consumer ring of trivially copyable items. Both
// sides are wait-free and do not allocate after construction.
#ifndef TSNX_UTIL_SPSC_RING_H_
#define TSNX_UTIL_SPSC_RING_H_

#include <atomic>
#include <cstddef>
#include <memory>
#include <type_traits>

namespace tsnx {

template <typename T>
class SpscRing {
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  // `capacity` is rounded up to a power of two.
  explicit SpscRing(size_t capacity) {
    size_t c = 1;
    while (c < capacity) c <<= 1;
    mask_ = c - 1;
    items_ = std::make_unique<T[]>(c);
  }

  // Producer. Returns false when full.
  bool Push(const T& item) {
    const size_t w = write_.load(std::memory_order_relaxed);
    if (w - read_.load(std::memory_order_acquire) > mask_) return false;
    items_[w & mask_] = item;
    write_.store(w + 1, std::memory_order_release);
    return true;
  }

  // Producer: a slot to fill in place, then Commit(). Null when full.
  T* Reserve() {
    const size_t w = write_.load(std::memory_order_relaxed);
    if (w - read_.load(std::memory_order_acquire) > mask_) return nullptr;
    return &items_[w & mask_];
  }
  void Commit() {
    write_.store(write_.load(std::memory_order_relaxed) + 1,
                 std::memory_order_release);
  }

  // Consumer.
  bool Pop(T* out) {
    const T* item = Peek();
    if (!item) return false;
    *out = *item;
    Drop();
    return true;
  }
  const T* Peek() const {
    const size_t r = read_.load(std::memory_order_relaxed);
    if (r == write_.load(std::memory_order_acquire)) return nullptr;
    return &items_[r & mask_];
  }
  void Drop() {
    read_.store(read_.load(std::memory_order_relaxed) + 1,
                std::memory_order_release);
  }

  size_t Size() const {
    return write_.load(std::memory_order_acquire) -
           read_.load(std::memory_order_acquire);
  }
  size_t Capacity() const { return mask_ + 1; }

 private:
  std::unique_ptr<T[]> items_;
  size_t mask_ = 0;
  alignas(64) std::atomic<size_t> write_{0};
  alignas(64) std::atomic<size_t> read_{0};
};

}  // namespace tsnx

#endif  // TSNX_UTIL_SPSC_RING_H_
