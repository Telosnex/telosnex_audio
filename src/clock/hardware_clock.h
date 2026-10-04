// Hardware sample-clock observations (ported from the Telosnex libwebrtc
// fork, tsnx_hardware_clock_api.patch). A device that can read its hardware
// position (ALSA, workplan step 7) feeds these to the drift servo.
#ifndef TSNX_CLOCK_HARDWARE_CLOCK_H_
#define TSNX_CLOCK_HARDWARE_CLOCK_H_

#include <cstdint>

namespace tsnx {

enum class AudioHardwareClockDirection { kPlayout, kCapture };

// `position_frames` is an unwrapped hardware-domain frame position;
// `generation` changes whenever recovery invalidates that position's origin.
struct AudioHardwareClockObservation {
  AudioHardwareClockDirection direction;
  int64_t monotonic_time_ns;
  int64_t position_frames;
  uint32_t sample_rate_hz;
  uint32_t generation;
};

}  // namespace tsnx

#endif  // TSNX_CLOCK_HARDWARE_CLOCK_H_
