## Unreleased

- `TelosnexAudio.microphonePermission()` and
  `TelosnexAudio.requestMicrophonePermission()`. Since WebRTC M144 the Apple
  audio engine no longer asks for the microphone itself, so on iOS capture
  failed with a device error when the app had not asked. Ask with
  `requestMicrophonePermission()` before `startCapture`.
- `startCapture` without the microphone permission fails with
  `AudioEngineException.noMicrophonePermission` (-9) on iOS, Android, macOS
  with Apple voice processing, and the web, instead of a device error or a
  browser exception.

## 0.1.1

- WebRTC audio processing from webrtc-sdk `m150_release` (was
  `m144_release`), with the Chromium `third_party` that its `DEPS` names.
  It includes the newer LiveKit fixes for the Apple audio engine and ALSA.
- WebRTC's mobile echo canceller (AECM), which telosnex_audio did not use,
  is no longer bundled: upstream removed it.
- The Windows build (MSVC) compiles the new WebRTC code.

## 0.1.0

- First release on pub.dev.
