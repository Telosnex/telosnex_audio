/// telosnex_audio: capture, echo cancellation, and a mixer with seekable,
/// rate-changeable playback tracks (ADR: telosnex/docs/design/
/// ADR_telosnex_audio.md).
library;

import 'src/engine_stub.dart'
    if (dart.library.ffi) 'src/engine_native.dart'
    as impl;
import 'src/types.dart';

export 'src/types.dart';

/// Opens and checks for the engine.
abstract final class TelosnexAudio {
  /// True when this build has the native engine for this platform.
  static bool get isSupported => impl.engineSupported;

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
