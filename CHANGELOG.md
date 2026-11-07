## 0.1.4

- `LICENSE` has only telosnex_audio's MIT license. The notices of the bundled code are in `NOTICES`, which Flutter reads in place of `LICENSE`, so the app's license page still shows all of them.
- Shorter package description.

## 0.1.3

- Requires `code_assets` 2.2, `hooks` 2.2 and `kiln` 0.2. With `code_assets` 1.x, the package could not be installed with current `objective_c` and `shared_preferences_foundation`.
- The build uses the complete iOS and macOS deployment version, such as 12.3, not only the major version.

## 0.1.2

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
