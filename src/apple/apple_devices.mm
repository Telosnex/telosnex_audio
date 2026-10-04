#include "apple/apple_devices.h"

#include "api/make_ref_counted.h"
#include "modules/audio_device/audio_engine_device.h"

namespace tsnx {
webrtc::scoped_refptr<webrtc::AudioDeviceModule> CreateAppleVoiceProcessingAdm(
    const webrtc::Environment& env) {
  return webrtc::make_ref_counted<webrtc::AudioEngineDevice>(
      env, /*voice_processing_bypassed=*/false);
}
}  // namespace tsnx
