// Microphone permission (TSNX_MIC_* in tsnx_audio.h).
//
// WebRTC M144 and later no longer ask for the microphone on Apple
// (webrtc-sdk #265): AudioEngineDevice only checks the status and fails
// when it is not granted. The app asks first, with Request (iOS, macOS) or
// the Android plugin (android/).
#ifndef TSNX_PERMISSION_H_
#define TSNX_PERMISSION_H_

#include <cstdint>
#include <functional>

namespace tsnx::permission {

inline constexpr int32_t kGranted = 0;
inline constexpr int32_t kDenied = 1;  // also restricted (Apple)
inline constexpr int32_t kNotDetermined = 2;

// iOS, macOS: AVCaptureDevice's status for audio. Android: RECORD_AUDIO;
// not granted is kNotDetermined, as Android does not say whether a request
// would show the prompt; kGranted until the plugin loaded the library.
// Elsewhere there is no per-app permission: kGranted.
int32_t Status();

// iOS, macOS: shows the system prompt when the status is kNotDetermined
// and calls `done` with the answer, on a thread of the system's choosing;
// it does not block. Elsewhere, and when the status is known, calls
// done(Status()) before returning.
void Request(std::function<void(int32_t)> done);

}  // namespace tsnx::permission

#endif  // TSNX_PERMISSION_H_
