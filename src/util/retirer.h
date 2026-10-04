// Deferred free for memory that the audio thread may still be reading.
//
// The audio thread brackets each render block with BeginBlock()/EndBlock(),
// which make the epoch odd inside a block. To free a pointer, another thread
// first unpublishes it (an atomic exchange to null), then calls Retire(). The
// memory is freed once the audio thread is outside the block that might have
// loaded the old pointer. The audio thread never keeps such a pointer across
// blocks.
#ifndef TSNX_UTIL_RETIRER_H_
#define TSNX_UTIL_RETIRER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace tsnx {

class Retirer {
 public:
  ~Retirer() { ReclaimAll(); }

  // Audio thread.
  void BeginBlock() {
    epoch_.fetch_add(1, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
  }
  void EndBlock() { epoch_.fetch_add(1, std::memory_order_release); }

  // Any non-audio thread, after the pointer is unpublished.
  void Retire(std::function<void()> free_fn) {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const uint64_t e = epoch_.load(std::memory_order_seq_cst);
    std::lock_guard<std::mutex> lock(mu_);
    pending_.push_back({e, std::move(free_fn)});
  }

  // Frees what is safe now. Call from a non-audio thread.
  void Reclaim() {
    std::vector<std::function<void()>> ready;
    {
      std::lock_guard<std::mutex> lock(mu_);
      const uint64_t now = epoch_.load(std::memory_order_acquire);
      for (size_t i = 0; i < pending_.size();) {
        const uint64_t e = pending_[i].epoch;
        if ((e & 1) == 0 || now != e) {
          ready.push_back(std::move(pending_[i].free_fn));
          pending_[i] = std::move(pending_.back());
          pending_.pop_back();
        } else {
          ++i;
        }
      }
    }
    for (auto& f : ready) f();
  }

  // Only when the audio thread can no longer run (engine shutdown).
  void ReclaimAll() {
    std::vector<Pending> all;
    {
      std::lock_guard<std::mutex> lock(mu_);
      all.swap(pending_);
    }
    for (auto& p : all) p.free_fn();
  }

  size_t PendingCount() {
    std::lock_guard<std::mutex> lock(mu_);
    return pending_.size();
  }

 private:
  struct Pending {
    uint64_t epoch;
    std::function<void()> free_fn;
  };
  std::atomic<uint64_t> epoch_{0};
  std::mutex mu_;
  std::vector<Pending> pending_;
};

}  // namespace tsnx

#endif  // TSNX_UTIL_RETIRER_H_
