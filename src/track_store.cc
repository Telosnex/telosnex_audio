#include "track_store.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "minimp3_ex.h"

namespace tsnx {

struct TrackStore::Mp3 {
  mp3dec_ex_t dec;
};

namespace {
size_t SlotsFor(Retention retention, const StoreLimits& l) {
  if (retention == Retention::kUnplayed)
    return static_cast<size_t>(l.ahead_seconds + 4);
  return static_cast<size_t>(l.max_seconds + 2);
}
}  // namespace

TrackStore::TrackStore(int sample_rate, int channels, Retention retention,
                       std::string spill_path, StoreLimits limits)
    : TrackStore(sample_rate, channels, retention, std::move(spill_path),
                 limits, SlotsFor(retention, limits)) {}

TrackStore::TrackStore(int sample_rate, int channels, Retention retention,
                       std::string spill_path, StoreLimits limits,
                       size_t slots)
    : rate_(sample_rate),
      channels_(channels),
      retention_(retention),
      limits_(limits),
      chunk_frames_(sample_rate),
      num_slots_(slots),
      spill_path_(std::move(spill_path)),
      slots_(new std::atomic<Chunk*>[slots]) {
  for (size_t i = 0; i < num_slots_; ++i) slots_[i].store(nullptr);
  if (retention_ == Retention::kAll) spilled_.assign(num_slots_, 0);
}

std::unique_ptr<TrackStore> TrackStore::OpenMp3(const uint8_t* data,
                                                size_t size,
                                                StoreLimits limits) {
  if (!data || size == 0) return nullptr;
  std::vector<uint8_t> bytes(data, data + size);
  auto* mp3 = new Mp3;
  std::memset(&mp3->dec, 0, sizeof(mp3->dec));
  mp3dec_ex_t* dec = &mp3->dec;
  if (mp3dec_ex_open_buf(dec, bytes.data(), bytes.size(),
                         MP3D_SEEK_TO_SAMPLE) != 0 ||
      dec->samples == 0 || dec->info.channels < 1 || dec->info.channels > 2 ||
      dec->info.hz <= 0 || dec->info.hz % 100 != 0) {
    mp3dec_ex_close(dec);
    delete mp3;
    return nullptr;
  }
  const int rate = dec->info.hz;
  const int channels = dec->info.channels;
  const int64_t frames = static_cast<int64_t>(dec->samples) / channels;
  const size_t chunks = static_cast<size_t>((frames + rate - 1) / rate) + 1;
  std::unique_ptr<TrackStore> store(new TrackStore(
      rate, channels, Retention::kAll, std::string(), limits, chunks));
  // The decoder keeps a pointer into the buffer; moving a vector keeps it.
  store->mp3_bytes_ = std::move(bytes);
  store->mp3_ = mp3;
  store->mp3_scratch_.resize(static_cast<size_t>(rate) * channels);
  store->written_.store(frames, std::memory_order_release);
  store->eos_.store(true, std::memory_order_release);
  return store;
}

TrackStore::~TrackStore() { ReleaseAll(); }

void TrackStore::ReleaseAll() {
  std::lock_guard<std::mutex> lock(mu_);
  for (size_t i = 0; i < num_slots_; ++i) {
    Chunk* c = slots_[i].exchange(nullptr);
    std::free(c);
  }
  if (spill_fd_ >= 0) {
#if defined(_WIN32)
    _close(spill_fd_);
#else
    close(spill_fd_);
#endif
    spill_fd_ = -1;
  }
  if (!spill_path_.empty()) std::remove(spill_path_.c_str());
  if (mp3_) {
    mp3dec_ex_close(&mp3_->dec);
    delete mp3_;
    mp3_ = nullptr;
  }
}

TrackStore::Chunk* TrackStore::AllocChunk(int64_t index) {
  const size_t bytes = offsetof(Chunk, data) +
                       static_cast<size_t>(chunk_frames_) * channels_ *
                           sizeof(int16_t);
  auto* c = static_cast<Chunk*>(std::malloc(bytes));
  c->index = index;
  return c;
}

TrackStore::Chunk* TrackStore::SlotChunk(int64_t index) const {
  Chunk* c = slots_[static_cast<size_t>(index) % num_slots_].load(
      std::memory_order_acquire);
  return (c && c->index == index) ? c : nullptr;
}

void TrackStore::Publish(int64_t index, Chunk* c) {
  slots_[static_cast<size_t>(index) % num_slots_].store(
      c, std::memory_order_release);
}

void TrackStore::RetireSlot(int64_t index, Retirer* retirer) {
  Chunk* c = slots_[static_cast<size_t>(index) % num_slots_].exchange(
      nullptr, std::memory_order_seq_cst);
  if (!c) return;
  retirer->Retire([c] { std::free(c); });
}

int64_t TrackStore::ChunkFramesAt(int64_t index) const {
  const int64_t w = written_.load(std::memory_order_relaxed);
  return std::clamp<int64_t>(w - index * chunk_frames_, 0, chunk_frames_);
}

int64_t TrackStore::Write(const int16_t* pcm, int64_t frames,
                          int64_t playhead, Retirer& retirer) {
  if (frames <= 0) return 0;
  std::lock_guard<std::mutex> lock(mu_);
  if (eos_.load(std::memory_order_relaxed) || mp3_) return kStoreEnded;
  if (failed_.load(std::memory_order_relaxed)) return kStoreIoError;
  int64_t w = written_.load(std::memory_order_relaxed);
  if (retention_ == Retention::kUnplayed) {
    if (w + frames - std::max<int64_t>(playhead, 0) >
        limits_.ahead_seconds * chunk_frames_)
      return kStoreFull;
  } else if (w + frames > limits_.max_seconds * chunk_frames_) {
    return kStoreFull;
  }
  int64_t done = 0;
  while (done < frames) {
    const int64_t k = w / chunk_frames_;
    const int64_t off = w % chunk_frames_;
    Chunk* c = SlotChunk(k);
    if (!c) {
      // A ring slot can still hold a played chunk that the control thread
      // has not freed yet.
      if (slots_[static_cast<size_t>(k) % num_slots_].load())
        RetireSlot(k % static_cast<int64_t>(num_slots_), &retirer);
      c = AllocChunk(k);
      Publish(k, c);
    }
    const int64_t n = std::min(chunk_frames_ - off, frames - done);
    std::memcpy(c->data + off * channels_, pcm + done * channels_,
                static_cast<size_t>(n * channels_) * sizeof(int16_t));
    done += n;
    w += n;
    written_.store(w, std::memory_order_release);
  }
  return frames;
}

void TrackStore::EndOfStream() {
  std::lock_guard<std::mutex> lock(mu_);
  eos_.store(true, std::memory_order_release);
}

int64_t TrackStore::Read(int64_t pos, int16_t* dst, int64_t max_frames) const {
  const int64_t end = std::min(pos + max_frames, written());
  int64_t n = 0;
  while (pos < end) {
    const int64_t k = pos / chunk_frames_;
    const int64_t off = pos % chunk_frames_;
    const Chunk* c = SlotChunk(k);
    if (!c) break;
    const int64_t m = std::min(chunk_frames_ - off, end - pos);
    std::memcpy(dst + n * channels_, c->data + off * channels_,
                static_cast<size_t>(m * channels_) * sizeof(int16_t));
    pos += m;
    n += m;
  }
  return n;
}

bool TrackStore::SpillChunk(Chunk* c) {
  if (spill_path_.empty()) return false;
  if (spill_fd_ < 0) {
#if defined(_WIN32)
    spill_fd_ = _open(spill_path_.c_str(),
                      _O_RDWR | _O_CREAT | _O_TRUNC | _O_BINARY, 0600);
#else
    spill_fd_ = open(spill_path_.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
#endif
    if (spill_fd_ < 0) return false;
  }
  const size_t bytes =
      static_cast<size_t>(ChunkFramesAt(c->index) * channels_) * 2;
  const int64_t offset = c->index * chunk_frames_ * channels_ * 2;
#if defined(_WIN32)
  if (_lseeki64(spill_fd_, offset, SEEK_SET) < 0) return false;
  return _write(spill_fd_, c->data, static_cast<unsigned>(bytes)) ==
         static_cast<int>(bytes);
#else
  return pwrite(spill_fd_, c->data, bytes, offset) ==
         static_cast<ssize_t>(bytes);
#endif
}

bool TrackStore::LoadChunk(int64_t index, Chunk* c) {
  const int64_t frames = ChunkFramesAt(index);
  const size_t samples = static_cast<size_t>(frames * channels_);
  if (mp3_) {
    if (mp3dec_ex_seek(&mp3_->dec, static_cast<uint64_t>(index * chunk_frames_ *
                                                   channels_)) != 0)
      return false;
    const size_t got = mp3dec_ex_read(&mp3_->dec, c->data, samples);
    if (got < samples)
      std::memset(c->data + got, 0, (samples - got) * sizeof(int16_t));
    return true;
  }
  if (spill_fd_ < 0) return false;
  const int64_t offset = index * chunk_frames_ * channels_ * 2;
#if defined(_WIN32)
  if (_lseeki64(spill_fd_, offset, SEEK_SET) < 0) return false;
  return _read(spill_fd_, c->data, static_cast<unsigned>(samples * 2)) ==
         static_cast<int>(samples * 2);
#else
  return pread(spill_fd_, c->data, samples * 2, offset) ==
         static_cast<ssize_t>(samples * 2);
#endif
}

bool TrackStore::Maintain(int64_t playhead, Retirer& retirer) {
  std::lock_guard<std::mutex> lock(mu_);
  const int64_t w = written_.load(std::memory_order_relaxed);
  if (w <= 0 || failed_.load(std::memory_order_relaxed)) return false;
  const bool eos = eos_.load(std::memory_order_relaxed);
  const int64_t last = (w - 1) / chunk_frames_;
  const int64_t pk = std::clamp<int64_t>(playhead / chunk_frames_, 0, last);
  bool changed = false;

  if (retention_ == Retention::kUnplayed) {
    for (size_t s = 0; s < num_slots_; ++s) {
      Chunk* c = slots_[s].load(std::memory_order_relaxed);
      if (c && c->index < pk) {
        RetireSlot(c->index, &retirer);
        changed = true;
      }
    }
    return changed;
  }

  const int64_t lo = pk - limits_.behind_seconds;
  const int64_t hi = pk + limits_.ahead_seconds;
  for (size_t s = 0; s < num_slots_; ++s) {
    Chunk* c = slots_[s].load(std::memory_order_relaxed);
    if (!c || (c->index >= lo && c->index <= hi)) continue;
    const int64_t k = c->index;
    if (!mp3_) {
      const bool complete = (k + 1) * chunk_frames_ <= w || eos;
      if (!complete) continue;  // the writer's tail chunk stays
      if (!spilled_[static_cast<size_t>(k)]) {
        if (!SpillChunk(c)) {
          failed_.store(true, std::memory_order_release);
          return true;
        }
        spilled_[static_cast<size_t>(k)] = 1;
      }
    }
    RetireSlot(k, &retirer);
    changed = true;
  }

  // Load nearest to the playhead first: pk, pk+1 .. hi, then pk-1 .. lo.
  auto load = [&](int64_t k) {
    if (k < 0 || k > last || SlotChunk(k)) return true;
    if (!mp3_ && !spilled_[static_cast<size_t>(k)]) return true;
    Chunk* c = AllocChunk(k);
    if (!LoadChunk(k, c)) {
      std::free(c);
      failed_.store(true, std::memory_order_release);
      return false;
    }
    Publish(k, c);
    changed = true;
    return true;
  };
  for (int64_t k = pk; k <= std::min(hi, last); ++k)
    if (!load(k)) return true;
  for (int64_t k = pk - 1; k >= std::max<int64_t>(lo, 0); --k)
    if (!load(k)) return true;
  return changed;
}

int64_t TrackStore::ResidentChunks() const {
  int64_t n = 0;
  for (size_t s = 0; s < num_slots_; ++s)
    if (slots_[s].load(std::memory_order_acquire)) ++n;
  return n;
}

}  // namespace tsnx
