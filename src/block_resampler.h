// Fixed-ratio resampler for exact 10 ms blocks of interleaved audio, built on
// WebRTC's SincResampler (one per channel). Unlike webrtc::PushResampler it
// can Reset() without allocating, so a seek or flush starts clean (I5).
#ifndef TSNX_BLOCK_RESAMPLER_H_
#define TSNX_BLOCK_RESAMPLER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace webrtc {
class SincResampler;
}

namespace tsnx {

class BlockResampler {
 public:
  // Rates must be multiples of 100. Allocates here only.
  BlockResampler(int src_rate, int dst_rate, int channels);
  ~BlockResampler();

  int src_frames() const { return src_frames_; }
  int dst_frames() const { return dst_frames_; }
  int channels() const { return channels_; }
  bool passthrough() const { return src_frames_ == dst_frames_; }

  // `src` holds src_frames() interleaved frames; `dst` receives
  // dst_frames() interleaved frames. Samples are float in int16 scale.
  void Process(const float* src, float* dst);
  void Reset();

  // Source-rate frames between input and output.
  static double DelayFrames();

 private:
  struct Channel;
  const int src_frames_;
  const int dst_frames_;
  const int channels_;
  std::vector<std::unique_ptr<Channel>> chans_;
  std::vector<float> in_;
  std::vector<float> out_;
};

}  // namespace tsnx

#endif  // TSNX_BLOCK_RESAMPLER_H_
