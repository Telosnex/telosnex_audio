/// telosnex_audio: capture, echo cancellation, and a mixer with seekable,
/// rate-changeable playback tracks (ADR: telosnex/docs/design/
/// ADR_telosnex_audio.md).
library;

import 'src/engine_stub.dart'
    if (dart.library.js_interop) 'src/engine_web.dart'
    if (dart.library.ffi) 'src/engine_native.dart'
    as impl;
import 'src/types.dart';

export 'src/types.dart';

/// Opens and checks for the engine.
abstract final class TelosnexAudio {
  /// True when this build has the engine for this platform: the native
  /// library, or on web, AudioWorklet and WebAssembly.
  static bool get isSupported => impl.engineSupported;

  /// Whether the app may use the microphone, without asking.
  ///
  /// iOS and macOS: the system's microphone setting of the app. Android:
  /// `RECORD_AUDIO`. Web: the Permissions API (`notDetermined` where the
  /// browser does not tell). Linux and Windows have no per-app permission
  /// to ask for: [MicrophonePermission.granted] (on Windows, the privacy
  /// setting can still silence or fail capture).
  static Future<MicrophonePermission> microphonePermission() =>
      impl.microphonePermission();

  /// Asks the user for the microphone if the app has not asked yet, and
  /// completes with the answer. Ask before [AudioEngine.startCapture]:
  /// since WebRTC M144, capture on iOS (and macOS with
  /// [PlatformDevice.appleVoiceProcessing]) fails with
  /// [AudioEngineException.noMicrophonePermission] instead of showing the
  /// prompt itself.
  ///
  /// The app declares the permission: `NSMicrophoneUsageDescription` in the
  /// Info.plist (iOS and macOS; without it the system ends the app), the
  /// `com.apple.security.device.audio-input` entitlement for a sandboxed
  /// macOS app, and `android.permission.RECORD_AUDIO` in the Android
  /// manifest (without it, the answer is [MicrophonePermission.denied]).
  ///
  /// Shows the prompt only in the foreground. Android needs an activity
  /// and the root isolate (a method channel); otherwise this throws a
  /// `PlatformException`. On the web, this opens and closes the microphone
  /// (getUserMedia) and throws [AudioEngineException] when there is none.
  /// Linux and Windows: [MicrophonePermission.granted] at once.
  static Future<MicrophonePermission> requestMicrophonePermission() =>
      impl.requestMicrophonePermission();

  static Future<AudioEngine> open({
    EngineDevice device = const PlatformDevice(),
    AudioProcessingConfig processing = const AudioProcessingConfig(),
    String? spillDir,
  }) => impl.openEngine(
    device: device,
    processing: processing,
    spillDir: spillDir,
  );
}
