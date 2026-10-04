import 'types.dart';

bool get engineSupported => false;

Future<AudioEngine> openEngine({
  required EngineDevice device,
  required AudioProcessingConfig processing,
  String? spillDir,
}) =>
    throw UnsupportedError('telosnex_audio is not available on this platform');
