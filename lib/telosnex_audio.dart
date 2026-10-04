/// telosnex_audio: capture, echo cancellation, and playback tracks.
library;

import 'package:ffi/ffi.dart';

import 'src/ffi.dart';

/// The native library version.
String nativeVersion() => tsnxVersion().toDartString();
