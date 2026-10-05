#include "track.h"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include "sonic.h"
int tsnx_sonic_pending_input_frames(sonicStream s);
void tsnx_sonic_reset(sonicStream s);
}

namespace tsnx {

namespace {
constexpr int64_t kBlockNs = 10'000'000;

int16_t ToS16(float v) {
  if (v > 32767.f) return 32767;
  if (v < -32768.f) return -32768;
  return static_cast<int16_t>(std::lrintf(v));
}
}  // namespace

Track::Track(int id, std::unique_ptr<TrackStore> store)
    : id_(id),
      store_(std::move(store)),
      src_frames_(store_->sample_rate() / 100),
      ramp_frames_(std::max(
          1, static_cast<int>(std::lround(store_->sample_rate() * kRampSeconds)))) {
  const int ch = channels();
  sonic_ = sonicCreateStream(sample_rate(), ch);
  resampler_ = std::make_unique<BlockResampler>(sample_rate(), kOutRate, ch);
  read_buf_.resize(static_cast<size_t>(src_frames_ * ch));
  sonic_out_.resize(static_cast<size_t>(src_frames_ * 4 * ch));
  f_src_.resize(static_cast<size_t>(src_frames_ * ch));
  f_dst_.resize(static_cast<size_t>(kOutFrames * ch));

  // Warm-up (ADR D8): grow every Sonic buffer to its worst case now, so the
  // audio thread never reallocates. Rates 0.5 and 3.0 are the API limits.
  std::vector<int16_t> zeros(static_cast<size_t>(sample_rate() / 10 * ch), 0);
  for (float speed : {0.5f, 3.0f, 1.0f}) {
    sonicSetSpeed(sonic_, speed);
    for (int i = 0; i < 5; ++i)
      sonicWriteShortToStream(sonic_, zeros.data(), sample_rate() / 10);
    sonicFlushStream(sonic_);
    while (sonicReadShortFromStream(sonic_, sonic_out_.data(),
                                    src_frames_ * 4) > 0) {
    }
  }
  sonicSetSpeed(sonic_, 1.0f);
  tsnx_sonic_reset(sonic_);

  TrackStateWords s{};
  s.status = static_cast<int64_t>(TrackStatus::kIdle);
  s.rate = 1.0;
  s.written = store_->written();
  s.duration = store_->ended() ? store_->written() : -1;
  state_.Store(s);
}

Track::~Track() {
  if (sonic_) sonicDestroyStream(sonic_);
}

double Track::PositionE2() const {
  const double held = tsnx_sonic_pending_input_frames(sonic_) +
                      sonicSamplesAvailable(sonic_) * rate_;
  double pos = static_cast<double>(fed_) - held;
  pos -= resampler_held_;
  return std::clamp(pos, 0.0, static_cast<double>(store_->written()));
}

void Track::ResetPipeline() {
  tsnx_sonic_reset(sonic_);
  resampler_->Reset();
  resampler_held_ = 0;
  sonic_flushed_ = false;
  end_rendered_ = false;
}

int Track::FillSonic(int need) {
  const int chunk = std::max(1, src_frames_ / 2);
  while (sonicSamplesAvailable(sonic_) < need) {
    const int64_t written = store_->written();
    if (fed_ < written) {
      const int64_t n = store_->Read(
          fed_, read_buf_.data(), std::min<int64_t>(chunk, written - fed_));
      if (n <= 0) break;  // not in memory yet: starved
      sonicWriteShortToStream(sonic_, read_buf_.data(), static_cast<int>(n));
      fed_ += n;
    } else if (store_->ended() && !sonic_flushed_) {
      sonicFlushStream(sonic_);
      sonic_flushed_ = true;
    } else {
      break;
    }
  }
  playhead_.store(fed_, std::memory_order_release);
  return sonicSamplesAvailable(sonic_);
}

void Track::SetStatus(TrackStatus s, EventSink& events) {
  if (s == status_) return;
  const TrackStatus old = status_;
  status_ = s;
  if (s == TrackStatus::kStarved && old == TrackStatus::kPlaying) {
    ++underruns_;
    events.PushRt({static_cast<int32_t>(TrackEventKind::kStarved), id_, 0});
  } else if (s == TrackStatus::kPlaying && old == TrackStatus::kStarved) {
    events.PushRt({static_cast<int32_t>(TrackEventKind::kResumed), id_, 0});
  } else if (s == TrackStatus::kFailed) {
    events.PushRt({static_cast<int32_t>(TrackEventKind::kFailed), id_, 0});
  }
}

// ---- Commands ----

void Track::Play() {
  if (status_ == TrackStatus::kFailed) return;
  if (status_ == TrackStatus::kEnded) return;  // seek first to play again
  if (want_play_) {
    if (cut_ == Cut::kPause) cut_ = Cut::kNone;
    return;
  }
  want_play_ = true;
  want_play_flag_.store(true, std::memory_order_release);
  fade_ = 0.0;
  fade_in_pending_ = true;
  started_pending_ = true;
  started_at_ns_ = 0;
  status_ = TrackStatus::kPlaying;
}

void Track::Pause() {
  if (!want_play_) return;
  if (status_ == TrackStatus::kPlaying && cut_ == Cut::kNone) {
    cut_ = Cut::kPause;
    return;
  }
  if (cut_ == Cut::kSeek || cut_ == Cut::kFlush) {
    // Fade-out already pending; pause after it.
    want_play_ = false;
    want_play_flag_.store(false, std::memory_order_release);
    return;
  }
  want_play_ = false;
  want_play_flag_.store(false, std::memory_order_release);
  started_pending_ = false;
  if (status_ != TrackStatus::kFailed) status_ = TrackStatus::kPaused;
}

void Track::Seek(int64_t frame) {
  if (status_ == TrackStatus::kFailed) return;
  frame = std::max<int64_t>(frame, 0);
  if (store_->ended()) frame = std::min(frame, store_->written());
  if (want_play_ && status_ == TrackStatus::kPlaying && !end_rendered_) {
    cut_ = Cut::kSeek;
    cut_target_ = frame;
    return;
  }
  ResetPipeline();
  fed_ = frame;
  last_e2_ = 0;
  playhead_.store(fed_, std::memory_order_release);
  heard_count_ = 0;
  if (status_ == TrackStatus::kEnded) status_ = TrackStatus::kPaused;
  if (want_play_) {
    fade_in_pending_ = true;
    fade_ = 0.0;
  }
}

void Track::SetRate(double rate) {
  rate_ = std::clamp(rate, 0.5, 3.0);
  sonicSetSpeed(sonic_, static_cast<float>(rate_));
}

void Track::SetGain(double gain, int64_t ramp_frames) {
  gain_target_ = std::clamp(gain, 0.0, 4.0);
  if (ramp_frames <= 0) {
    gain_ = gain_target_;
    gain_step_ = 0;
  } else {
    gain_step_ = (gain_target_ - gain_) / static_cast<double>(ramp_frames);
  }
}

void Track::Flush(int64_t target_written) {
  flush_target_ = std::max<int64_t>(target_written, 0);
  if (want_play_ && status_ == TrackStatus::kPlaying && !end_rendered_) {
    cut_ = Cut::kFlush;
    return;
  }
  flush_now_ = true;  // applied at the next Render, which can push events
}

void Track::OnOutputRestart(int64_t stop_ns) {
  if (!want_play_ || status_ == TrackStatus::kFailed ||
      status_ == TrackStatus::kEnded || heard_count_ == 0)
    return;
  const int64_t heard = HeardPosition(stop_ns);
  if (heard >= MonotonicE2() && !end_rendered_) return;  // nothing lost
  ResetPipeline();
  fed_ = heard;
  last_e2_ = heard;
  heard_count_ = 0;
  playhead_.store(fed_, std::memory_order_release);
  if (started_pending_) started_at_ns_ = 0;  // the start was never heard
  fade_in_pending_ = true;
  fade_ = 0.0;
}

// ---- Render ----

void Track::PushHeard(int64_t t_heard_ns, int64_t pos_start, int64_t pos_end) {
  heard_[heard_head_] = {t_heard_ns, pos_start, pos_end};
  heard_head_ = (heard_head_ + 1) % kHeardRing;
  heard_count_ = std::min(heard_count_ + 1, kHeardRing);
}

int64_t Track::HeardPosition(int64_t now_ns) const {
  // Newest entry whose block has started to be heard.
  for (int i = 1; i <= heard_count_; ++i) {
    const HeardEntry& e = heard_[(heard_head_ - i + kHeardRing) % kHeardRing];
    if (e.t_heard_ns <= now_ns) {
      const double f =
          std::min(1.0, static_cast<double>(now_ns - e.t_heard_ns) / kBlockNs);
      return e.pos_start + static_cast<int64_t>(
                               std::llround(f * (e.pos_end - e.pos_start)));
    }
  }
  if (heard_count_ == 0) return static_cast<int64_t>(PositionE2());
  // Nothing heard yet: the oldest entry's start.
  const int oldest = (heard_head_ - heard_count_ + kHeardRing) % kHeardRing;
  return heard_[oldest].pos_start;
}

int64_t Track::MonotonicE2() {
  const int64_t e2 = static_cast<int64_t>(std::floor(PositionE2()));
  last_e2_ = std::max(last_e2_, e2);
  return last_e2_;
}

bool Track::Render(int16_t* out, int64_t mix_time_ns, int64_t device_delay_ns,
                   EventSink& events) {
  device_delay_ns_ = device_delay_ns;
  const int64_t t_heard = mix_time_ns + device_delay_ns;
  const int ch = channels();

  if (status_ != TrackStatus::kFailed && store_->failed()) {
    want_play_ = false;
    want_play_flag_.store(false, std::memory_order_release);
    SetStatus(TrackStatus::kFailed, events);
  }
  ApplyPendingFlush(events);

  const bool audible_state =
      want_play_ && status_ != TrackStatus::kFailed && !end_rendered_;
  if (!audible_state) {
    const int64_t p = MonotonicE2();
    PushHeard(t_heard, p, p);
    return false;
  }

  const int need = src_frames_;
  const int avail = FillSonic(need);
  const int64_t pos_start = MonotonicE2();
  const bool cutting = cut_ != Cut::kNone;
  const int to_read = cutting ? std::min(ramp_frames_, need) : need;
  const int n = avail > 0 ? sonicReadShortFromStream(sonic_, sonic_out_.data(),
                                                     std::min(avail, to_read))
                          : 0;

  // Status from what was available.
  if (n == 0 && !cutting) {
    if (store_->ended() && fed_ >= store_->written() && sonic_flushed_) {
      end_rendered_ = true;
      end_heard_ns_ = t_heard;
    } else {
      SetStatus(TrackStatus::kStarved, events);
      fade_in_pending_ = true;
      fade_ = 0.0;
    }
  } else if (n > 0 && status_ == TrackStatus::kStarved) {
    SetStatus(TrackStatus::kPlaying, events);
  }
  if (n > 0 && n < need && !cutting && store_->ended() &&
      fed_ >= store_->written() && sonic_flushed_ &&
      sonicSamplesAvailable(sonic_) == 0) {
    end_rendered_ = true;
    end_heard_ns_ = t_heard + kBlockNs * n / need;
  }
  if (n > 0 && started_pending_ && started_at_ns_ == 0)
    started_at_ns_ = t_heard;

  // Ramps and gain, at the source rate.
  const double fade_step = 1.0 / ramp_frames_;
  for (int i = 0; i < n; ++i) {
    if (cutting) {
      fade_ = std::max(0.0, fade_ - fade_step);
    } else if (fade_in_pending_) {
      fade_ = std::min(1.0, fade_ + fade_step);
      if (fade_ >= 1.0) fade_in_pending_ = false;
    }
    if (gain_step_ != 0.0) {
      gain_ += gain_step_;
      if ((gain_step_ > 0 && gain_ >= gain_target_) ||
          (gain_step_ < 0 && gain_ <= gain_target_)) {
        gain_ = gain_target_;
        gain_step_ = 0.0;
      }
    }
    const float g = static_cast<float>(fade_ * gain_);
    for (int c = 0; c < ch; ++c)
      f_src_[i * ch + c] = sonic_out_[i * ch + c] * g;
  }
  std::fill(f_src_.begin() + n * ch, f_src_.begin() + need * ch, 0.f);
  resampler_->Process(f_src_.data(), f_dst_.data());
  if (!resampler_->passthrough()) {
    // Zero padding after the real samples pushes held frames out.
    const double d = BlockResampler::DelayFrames();
    resampler_held_ = n == need ? d : std::max(0.0, d - (need - n));
  }
  for (int i = 0; i < kOutFrames * ch; ++i) out[i] = ToS16(f_dst_[i]);

  const int64_t pos_end = MonotonicE2();
  PushHeard(t_heard, pos_start, pos_end);

  if (cutting) {
    const Cut cut = cut_;
    cut_ = Cut::kNone;
    fade_ = 0.0;
    if (cut == Cut::kPause) {
      want_play_ = false;
      want_play_flag_.store(false, std::memory_order_release);
      started_pending_ = false;
      status_ = TrackStatus::kPaused;
    } else {
      if (cut == Cut::kFlush) {
        events.PushRt({static_cast<int32_t>(TrackEventKind::kFlushed), id_,
                       pos_end});
        cut_target_ = flush_target_;
      }
      ResetPipeline();
      fed_ = cut_target_;
      last_e2_ = fed_;
      playhead_.store(fed_, std::memory_order_release);
      if (want_play_) {
        fade_in_pending_ = true;
        status_ = TrackStatus::kPlaying;
      } else {
        status_ = TrackStatus::kPaused;
      }
    }
  }
  return true;
}

void Track::ApplyPendingFlush(EventSink& events) {
  if (!flush_now_) return;
  flush_now_ = false;
  const int64_t cut = MonotonicE2();
  ResetPipeline();
  fed_ = flush_target_;
  last_e2_ = fed_;
  heard_count_ = 0;
  playhead_.store(fed_, std::memory_order_release);
  events.PushRt({static_cast<int32_t>(TrackEventKind::kFlushed), id_, cut});
}

void Track::Publish(int64_t now_ns, EventSink& events) {
  ApplyPendingFlush(events);
  if (started_pending_ && started_at_ns_ != 0 && now_ns >= started_at_ns_) {
    started_pending_ = false;
    events.PushRt({static_cast<int32_t>(TrackEventKind::kStarted), id_, 0});
  }
  if (end_rendered_ && status_ != TrackStatus::kEnded &&
      now_ns >= end_heard_ns_) {
    status_ = TrackStatus::kEnded;
    want_play_ = false;
    want_play_flag_.store(false, std::memory_order_release);
    started_pending_ = false;
    last_e2_ = store_->written();
    events.PushRt({static_cast<int32_t>(TrackEventKind::kEnded), id_,
                   store_->written()});
  }
  TrackStateWords s{};
  s.status = static_cast<int64_t>(status_);
  s.position = status_ == TrackStatus::kEnded ? store_->written()
                                              : HeardPosition(now_ns);
  s.sampled_at_ns = now_ns;
  s.rate = rate_;
  s.written = store_->written();
  s.duration = store_->ended() ? store_->written() : -1;
  s.mixer_position = last_e2_;
  s.underruns = underruns_;
  state_.Store(s);
}

}  // namespace tsnx
