// Track audio storage (ADR D11).
//
// A track's source audio is a sequence of 1 s chunks. Only a window around
// the playhead is in memory: at most 30 s ahead and 30 s behind. The audio
// thread reads resident chunks without locks. The writer (the Dart thread)
// and the control thread change the store under a mutex.
//
// Retention `.unplayed` frees chunks more than one chunk behind the playhead
// and refuses writes more than 30 s ahead. Retention `.all` keeps a backing store so seek works
// on the whole track: a spill file for streamed PCM, or the MP3 bytes.
//
// An external store (web) has its backing store outside: the host keeps a
// copy of every write, or the MP3 bytes. Maintain() asks for the chunks it
// lacks inside the window (TakeRequests), and the host gives them back
// (Deliver), possibly much later.
#ifndef TSNX_TRACK_STORE_H_
#define TSNX_TRACK_STORE_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "util/retirer.h"

namespace tsnx {

enum class Retention : int32_t { kAll = 0, kUnplayed = 1 };

enum StoreError : int64_t {
  kStoreFull = -1,         // .unplayed more than 30 s ahead, or .all past 2 h
  kStoreEnded = -2,        // write after endOfStream
  kStoreIoError = -3,      // spill file failure
};

struct StoreLimits {
  int64_t ahead_seconds = 30;
  int64_t behind_seconds = 30;
  int64_t max_seconds = 2 * 60 * 60;
};

class TrackStore {
 public:
  // Streamed PCM16 store. `spill_path` is used only for Retention::kAll.
  TrackStore(int sample_rate, int channels, Retention retention,
             std::string spill_path, StoreLimits limits = {});
  // MP3 store over a copy of `data`. Returns null if the data does not
  // decode or the format is not supported.
  static std::unique_ptr<TrackStore> OpenMp3(const uint8_t* data, size_t size,
                                             StoreLimits limits = {});
  // Retention::kAll store with an external backing store. `frames` >= 0: the
  // whole track exists there already (MP3) and no chunk is in memory yet.
  // `frames` < 0: streamed; writes come here and to the host.
  static std::unique_ptr<TrackStore> External(int sample_rate, int channels,
                                              int64_t frames,
                                              StoreLimits limits = {});
  ~TrackStore();

  int sample_rate() const { return rate_; }
  int channels() const { return channels_; }
  Retention retention() const { return retention_; }
  bool is_mp3() const { return mp3_ != nullptr; }
  bool external() const { return external_; }

  // Writer thread. Returns frames written (all of them) or a StoreError.
  int64_t Write(const int16_t* interleaved, int64_t frames, int64_t playhead,
                Retirer& retirer);
  void EndOfStream();

  // Any thread.
  int64_t written() const { return written_.load(std::memory_order_acquire); }
  bool ended() const { return eos_.load(std::memory_order_acquire); }
  bool failed() const { return failed_.load(std::memory_order_acquire); }

  // Audio thread. Copies resident frames starting at `pos` into `dst`, up to
  // `max_frames`. Stops at the first frame that is not written or not in
  // memory. Lock-free and allocation-free.
  int64_t Read(int64_t pos, int16_t* dst, int64_t max_frames) const;

  // Control thread. Moves the window to `playhead`: loads chunks from the
  // backing store, spills and frees chunks outside the window. Returns true
  // if it changed anything.
  bool Maintain(int64_t playhead, Retirer& retirer);

  // External stores, control thread. Moves the chunk indexes that Maintain
  // asked for into `out`. Each index is asked once until it is delivered.
  void TakeRequests(std::vector<int64_t>* out);
  // External stores, control thread. Publishes a chunk from the host if it
  // is still inside the window and not in memory. Returns true if it did.
  // `frames` must be the chunk's full length (shorter only for the last).
  bool Deliver(int64_t index, const int16_t* data, int64_t frames);
  // The host lost the backing store. Reads stop; the track fails.
  void Fail() { failed_.store(true, std::memory_order_release); }

  // Frees everything. The caller guarantees the audio thread no longer
  // reads this store.
  void ReleaseAll();

  // Test and diagnostics.
  int64_t ResidentChunks() const;
  int64_t chunk_frames() const { return chunk_frames_; }
  const std::string& spill_path() const { return spill_path_; }

 private:
  struct Chunk {
    int64_t index;
    int16_t data[1];  // chunk_frames_ * channels_ samples
  };
  TrackStore(int sample_rate, int channels, Retention retention,
             std::string spill_path, StoreLimits limits, size_t slots);

  Chunk* AllocChunk(int64_t index);
  Chunk* SlotChunk(int64_t index) const;
  void Publish(int64_t index, Chunk* c);
  void RetireSlot(int64_t index, Retirer* retirer);
  bool SpillChunk(Chunk* c);
  bool LoadChunk(int64_t index, Chunk* c);
  int64_t ChunkFramesAt(int64_t index) const;

  const int rate_;
  const int channels_;
  const Retention retention_;
  const StoreLimits limits_;
  const int64_t chunk_frames_;
  const size_t num_slots_;
  std::string spill_path_;

  std::unique_ptr<std::atomic<Chunk*>[]> slots_;
  std::atomic<int64_t> written_{0};
  std::atomic<bool> eos_{false};
  std::atomic<bool> failed_{false};

  mutable std::mutex mu_;
  std::vector<uint8_t> spilled_;  // .all PCM: chunk is in the spill file
  int spill_fd_ = -1;

  std::vector<uint8_t> mp3_bytes_;
  struct Mp3;
  Mp3* mp3_ = nullptr;
  std::vector<int16_t> mp3_scratch_;

  bool external_ = false;
  std::vector<uint8_t> requested_;  // external: index asked, not delivered
  std::vector<int64_t> requests_;   // external: asked since TakeRequests
  int64_t window_lo_ = 0;           // external: window at the last Maintain
  int64_t window_hi_ = -1;
};

}  // namespace tsnx

#endif  // TSNX_TRACK_STORE_H_
