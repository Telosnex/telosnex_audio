// Android audio device on AAudio (ADR workplan step 8).
//
// Output is media audio (USAGE_MEDIA): media volume and normal routing, also
// while the microphone runs (ADR D6 on Android). The microphone uses the
// VOICE_RECOGNITION preset, which has no platform echo canceller; the APM's
// AEC3 removes the echo.
//
// Devices are the routes of android_routes.h, from AudioManager through
// JNI. The earpiece and the Bluetooth microphone put Android in
// communication mode with USAGE_VOICE_COMMUNICATION output, only while one
// of them is selected and a stream is open. PlayoutDevices() reads the
// device list again when Android reported a change, and moves open streams
// when the route for the selection changed (a headset left or came back).
//
// AAudio loads at run time, so the library also loads on Android 7. There,
// Init() fails and the engine reports no device.
#ifndef TSNX_ANDROID_AAUDIO_DEVICE_H_
#define TSNX_ANDROID_AAUDIO_DEVICE_H_

#include <functional>

#include "api/audio/audio_device.h"
#include "api/environment/environment.h"
#include "api/scoped_refptr.h"
#include "device_info.h"

namespace tsnx {

// The routes in use for the selection (android_routes::CurrentOutput and
// CurrentInput). Device thread.
class AAudioRoutes {
 public:
  virtual DeviceInfo CurrentOutput() = 0;
  virtual DeviceInfo CurrentInput() = 0;

 protected:
  ~AAudioRoutes() = default;
};

// `on_output_restart` runs (on any thread) when the output stream is lost
// and reopens, for example after a headset is plugged in. The audio in the
// old stream's buffer was never heard (ADR I10). `routes` is valid while
// the module lives.
webrtc::scoped_refptr<webrtc::AudioDeviceModule> CreateAAudioAdm(
    const webrtc::Environment& env, std::function<void()> on_output_restart,
    AAudioRoutes** routes);

}  // namespace tsnx

#endif  // TSNX_ANDROID_AAUDIO_DEVICE_H_
