// One playback source in the engine mixer (ADR D7-D11).
//
// Render path, on the audio thread, one 10 ms block at a time:
//   store -> Sonic (source rate, time stretch) -> ramps and gain
//         -> BlockResampler -> 48 kHz frame for the mixer
//
// The audio thread is the only thread that touches Sonic, the resampler,
// the ramps, and the status. Other threads talk to a Track through engine
// commands (applied at the start of a block) and read its state through a
// seqlock.
#ifndef TSNX_TRACK_H_
#define TSNX_TRACK_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "block_resampler.h"
#include "track_store.h"
#include "util/seqlock.h"

struct sonicStreamStruct;

namespace tsnx {

enum class TrackStatus : int32_t {
  kIdle = 0,
  kPlaying = 1,
  kPaused = 2,
  kStarved = 3,
  kEnded = 4,
  kFailed = 5,
};

enum class TrackEventKind : int32_t {
  kStarted = 1,
  kStarved = 2,
  kResumed = 3,
  kEnded = 4,
  kFailed = 5,
  kFlushed = 6,  // value: cut position in source frames
};

// Published after each block. 8-byte words only (Seqlock requirement).
struct TrackStateWords {
  int64_t status;
  int64_t position;      // heard position, source frames
  int64_t sampled_at_ns;
  double rate;
  int64_t written;       // source frames written
  int64_t duration;      // source frames, or -1 before endOfStream
  int64_t mixer_position;  // source frames at the mixer (before device delay)
  int64_t underruns;     // blocks that were starved while playing
};

struct TrackEvent {
  int32_t kind;
  int32_t track_id;
  int64_t value;
};

class EventSink {
 public:
  virtual ~EventSink() = default;
  // Audio thread. Must not block or allocate.
  virtual void PushRt(const TrackEvent& e) = 0;
};

class Track {
 public:
  static constexpr int kOutRate = 48000;
  static constexpr int kOutFrames = kOutRate / 100;
  static constexpr double kRampSeconds = 0.005;

  Track(int id, std::unique_ptr<TrackStore> store);
  ~Track();

  int id() const { return id_; }
  TrackStore& store() { return *store_; }
  const TrackStore& store() const { return *store_; }
  int sample_rate() const { return store_->sample_rate(); }
  int channels() const { return store_->channels(); }

  // ---- Commands, audio thread (applied by the engine) ----
  void Play();
  void Pause();
  void Seek(int64_t frame);
  void SetRate(double rate);
  void SetGain(double gain, int64_t ramp_frames);
  void Flush(int64_t target_written);
  // The output device stopped at `stop_ns` and started again; audio rendered
  // after that was never heard. Moves a playing track back to the position
  // heard at `stop_ns` (ADR I10).
  void OnOutputRestart(int64_t stop_ns);

  // ---- Render, audio thread ----
  // Fills `out` (kOutFrames * channels() samples, 48 kHz interleaved).
  // Returns false if the track is silent this block (kMuted).
  bool Render(int16_t* out, int64_t mix_time_ns, int64_t device_delay_ns,
              EventSink& events);
  // After all tracks rendered: emit delayed events and publish state.
  void Publish(int64_t now_ns, EventSink& events);
  // Source frame the store must have next. Read by the control thread.
  int64_t playhead() const { return playhead_.load(std::memory_order_acquire); }
  bool wants_output() const {
    return want_play_flag_.load(std::memory_order_acquire);
  }

  // ---- Any thread ----
  TrackStateWords State() const { return state_.Load(); }

 private:
  struct HeardEntry {
    int64_t t_heard_ns;  // when the block's first sample is heard
    int64_t pos_start;   // source frame at the block's first sample
    int64_t pos_end;     // source frame after the block
  };

  double PositionE2() const;
  void ApplyPendingFlush(EventSink& events);
  void ResetPipeline();
  int FillSonic(int need);
  void SetStatus(TrackStatus s, EventSink& events);
  void PushHeard(int64_t t_heard_ns, int64_t pos_start, int64_t pos_end);
  int64_t MonotonicE2();
  int64_t HeardPosition(int64_t now_ns) const;

  const int id_;
  std::unique_ptr<TrackStore> store_;
  sonicStreamStruct* sonic_ = nullptr;
  std::unique_ptr<BlockResampler> resampler_;
  const int src_frames_;  // source frames per 10 ms
  const int ramp_frames_;

  // Audio-thread state.
  TrackStatus status_ = TrackStatus::kIdle;
  bool want_play_ = false;
  int64_t fed_ = 0;            // source frames given to Sonic
  bool sonic_flushed_ = false;
  double rate_ = 1.0;
  double gain_ = 1.0;
  double gain_target_ = 1.0;
  double gain_step_ = 0.0;
  double fade_ = 0.0;          // 0..1 transition ramp
  bool fade_in_pending_ = false;
  // A command that cuts the audio: fade out this block, then act.
  enum class Cut { kNone, kPause, kSeek, kFlush } cut_ = Cut::kNone;
  int64_t cut_target_ = 0;
  int64_t flush_target_ = 0;
  bool flush_now_ = false;
  int64_t last_e2_ = 0;  // I4: position does not decrease between seeks
  double resampler_held_ = 0;  // real source frames inside the resampler
  int64_t underruns_ = 0;
  bool started_pending_ = false;
  int64_t started_at_ns_ = 0;
  bool end_rendered_ = false;
  int64_t end_heard_ns_ = 0;
  int64_t device_delay_ns_ = 0;

  std::vector<int16_t> read_buf_;
  std::vector<int16_t> sonic_out_;
  std::vector<float> f_src_;
  std::vector<float> f_dst_;

  static constexpr int kHeardRing = 128;
  HeardEntry heard_[kHeardRing];
  int heard_count_ = 0;
  int heard_head_ = 0;

  std::atomic<int64_t> playhead_{0};
  std::atomic<bool> want_play_flag_{false};
  Seqlock<TrackStateWords> state_;
};

}  // namespace tsnx

#endif  // TSNX_TRACK_H_
