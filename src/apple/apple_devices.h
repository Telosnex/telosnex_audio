#ifndef TSNX_APPLE_APPLE_DEVICES_H_
#define TSNX_APPLE_APPLE_DEVICES_H_

#include "api/audio/audio_device.h"
#include "api/environment/environment.h"
#include "api/scoped_refptr.h"

namespace tsnx {
// AVAudioEngine device with Apple voice processing (VPIO). This is the
// device the fork uses on macOS and iOS.
webrtc::scoped_refptr<webrtc::AudioDeviceModule> CreateAppleVoiceProcessingAdm(
    const webrtc::Environment& env);
}  // namespace tsnx

#endif  // TSNX_APPLE_APPLE_DEVICES_H_
