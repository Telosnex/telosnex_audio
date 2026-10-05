import 'dart:typed_data';

import 'types.dart';

final class WebOutputTap {
  WebOutputTap._();
  int get sampleRate => throw UnsupportedError('web only');
  List<(int, Float32List)> get pieces => throw UnsupportedError('web only');
  void start() => throw UnsupportedError('web only');
  void stop() => throw UnsupportedError('web only');
}

Future<WebOutputTap> debugTapOutput(AudioEngine engine) =>
    throw UnsupportedError('web only');
void debugSetOutputDelay(AudioEngine engine, Duration delay) =>
    throw UnsupportedError('web only');
String debugContextState(AudioEngine engine) =>
    throw UnsupportedError('web only');
Future<void> debugResume(AudioEngine engine) =>
    throw UnsupportedError('web only');
void debugIdle(AudioEngine engine) => throw UnsupportedError('web only');
