// On-device checks of the platform engine (ADR workplan steps 7-8), as an
// app entry point so it runs without a debugger:
//
//   flutter build ios --profile -t lib/device_check.dart
//   xcrun devicectl device install app --device <id> build/ios/iphoneos/Runner.app
//   xcrun devicectl device process launch --console --device <id> <bundle id>
//
// Lines start with TSNX_DEVICE; the last one is PASS or FAIL.
import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:telosnex_audio/telosnex_audio.dart';

void log(String s) => stdout.writeln('TSNX_DEVICE: $s');

Future<void> sleep(int ms) => Future.delayed(Duration(milliseconds: ms));

final failures = <String>[];
void check(bool ok, String what) {
  log('${ok ? 'ok  ' : 'FAIL'} $what');
  if (!ok) failures.add(what);
}

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(
    const MaterialApp(
      home: Scaffold(body: Center(child: Text('device check'))),
    ),
  );
  try {
    await run();
  } catch (e, st) {
    failures.add('$e');
    log('exception $e\n$st');
  }
  log(failures.isEmpty ? 'PASS' : 'FAIL ${failures.length}');
  await sleep(500);
  exit(failures.isEmpty ? 0 : 1);
}

Future<void> run() async {
  final engine = await TelosnexAudio.open();
  log('outputs: ${engine.outputs.map((d) => '${d.id}=${d.name}').join('; ')}');
  log('inputs: ${engine.inputs.map((d) => '${d.id}=${d.name}').join('; ')}');

  final mp3 = await rootBundle.load('assets/speech.mp3');
  final track = await engine.createMp3Track(mp3.buffer.asUint8List());
  track
    ..setGain(0.15)
    ..play();
  final sw = Stopwatch()..start();
  await sleep(1500);
  var s = track.state;
  log(
    'after 1.5 s: ${s.status.name} at ${s.position.inMilliseconds} ms, '
    'delay ${engine.outputDelay.inMilliseconds} ms',
  );
  check(s.status == TrackStatus.playing, 'plays');
  check(
    s.position.inMilliseconds >= 800 && s.position.inMilliseconds <= 1600,
    'position follows wall time',
  );

  // Mic on: the communication profile restarts the output (D6, I10).
  var frames = 0;
  final sub = engine.capture.listen((f) => frames += f.pcm.length);
  final p0 = track.state.position;
  final t0 = sw.elapsedMilliseconds;
  await engine.startCapture(const CaptureConfig());
  await sleep(2000);
  final played = (track.state.position - p0).inMilliseconds;
  final wall = sw.elapsedMilliseconds - t0;
  log(
    'mic on: captured ${frames ~/ 48} ms in $wall ms; track moved $played ms '
    '(gap ${wall - played} ms); delay ${engine.outputDelay.inMilliseconds} ms',
  );
  check(frames > 48 * 1000, 'capture delivers audio');
  check(played <= wall + 60, 'no skip at mic on');
  check(played >= wall - 1500, 'restart gap under 1.5 s at mic on');
  check(track.state.status == TrackStatus.playing, 'still playing');

  final p2 = track.state.position;
  final t2 = sw.elapsedMilliseconds;
  await engine.stopCapture();
  await sub.cancel();
  await sleep(1500);
  final played2 = (track.state.position - p2).inMilliseconds;
  final wall2 = sw.elapsedMilliseconds - t2;
  log('mic off: track moved $played2 ms in $wall2 ms');
  check(played2 <= wall2 + 60, 'no skip at mic off');

  track.pause();
  await sleep(300);
  final paused = track.state.position;
  await sleep(500);
  check(track.state.position == paused, 'pause holds position');
  track
    ..seek(const Duration(seconds: 10))
    ..setRate(2)
    ..play();
  await sleep(1000);
  s = track.state;
  log('2x from 10 s for 1 s: ${s.position.inMilliseconds} ms');
  check(
    s.position.inMilliseconds >= 10800 && s.position.inMilliseconds <= 12200,
    'seek and 2x rate',
  );
  await track.dispose();
  await engine.close();
}
