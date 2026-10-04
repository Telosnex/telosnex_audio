#include "block_resampler.h"

#include <cstring>

#include "common_audio/resampler/sinc_resampler.h"

namespace tsnx {

struct BlockResampler::Channel : public webrtc::SincResamplerCallback {
  Channel(int src_frames, int dst_frames)
      : rs(static_cast<double>(src_frames) / dst_frames, src_frames, this) {}

  void Run(size_t frames, float* destination) override {
    if (first_pass) {
      std::memset(destination, 0, frames * sizeof(float));
      first_pass = false;
      return;
    }
    std::memcpy(destination, source, frames * sizeof(float));
  }

  webrtc::SincResampler rs;
  const float* source = nullptr;
  bool first_pass = true;
};

BlockResampler::BlockResampler(int src_rate, int dst_rate, int channels)
    : src_frames_(src_rate / 100),
      dst_frames_(dst_rate / 100),
      channels_(channels) {
  if (!passthrough()) {
    for (int c = 0; c < channels_; ++c)
      chans_.push_back(std::make_unique<Channel>(src_frames_, dst_frames_));
    in_.resize(static_cast<size_t>(src_frames_));
    out_.resize(static_cast<size_t>(dst_frames_));
  }
}

BlockResampler::~BlockResampler() = default;

double BlockResampler::DelayFrames() {
  return webrtc::SincResampler::kKernelSize / 2.0;
}

void BlockResampler::Process(const float* src, float* dst) {
  if (passthrough()) {
    std::memcpy(dst, src, sizeof(float) * src_frames_ * channels_);
    return;
  }
  for (int c = 0; c < channels_; ++c) {
    for (int i = 0; i < src_frames_; ++i) in_[i] = src[i * channels_ + c];
    Channel& ch = *chans_[c];
    ch.source = in_.data();
    if (ch.first_pass) ch.rs.Resample(ch.rs.ChunkSize(), out_.data());
    ch.rs.Resample(dst_frames_, out_.data());
    ch.source = nullptr;
    for (int i = 0; i < dst_frames_; ++i) dst[i * channels_ + c] = out_[i];
  }
}

void BlockResampler::Reset() {
  for (auto& ch : chans_) {
    ch->rs.Flush();
    ch->first_pass = true;
  }
}

}  // namespace tsnx
