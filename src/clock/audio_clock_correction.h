#ifndef TSNX_CLOCK_AUDIO_CLOCK_CORRECTION_H_
#define TSNX_CLOCK_AUDIO_CLOCK_CORRECTION_H_

#include <memory>
#include "clock/capture_clock_policy.h"
#include "clock/drift_servo.h"

namespace tsnx {

// Ported from the fork's include/rtc_audio_clock_correction.h.
enum class AudioClockCorrectionMode { kOff = 0, kObserve = 1, kControl = 2 };
struct AudioClockCorrectionState {
  // 0 off, 1 observe, 2 control, 3 legacy callback-only environment policy.
  int mode = 0;
  bool supported = false;  // hardware producer currently available only on ALSA
  bool capture_started = false;
  bool engaged = false;
  bool hardware_ready = false;
  bool hardware_controlling = false;
  double applied_ppm = 0;
};

class AudioClockCorrection {
 public:
  using Policy = CaptureClockPolicy<tsnx::DriftServo>;
  explicit AudioClockCorrection(bool supported);
  int Configure(AudioClockCorrectionMode mode);
  AudioClockCorrectionState GetState();
  Policy::Snapshot Read(bool capture = false) { return policy_->Read(capture); }
  double seed_ppm() const { return seed_ppm_; }

 private:
  const bool supported_;
  double seed_ppm_ = 0;
  std::unique_ptr<Policy> policy_;
};
}  // namespace tsnx
#endif
