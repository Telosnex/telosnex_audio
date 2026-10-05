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

// debugPrint reaches logcat on Android and the console on iOS.
void log(String s) => debugPrint('TSNX_DEVICE: $s');

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
  // Android: the plugin loads the library for JNI in the background, and
  // the engine reads the routes when it is ready.
  for (var i = 0; i < 20 && engine.outputs.length < 2; i++) {
    await sleep(100);
  }
  log('outputs: ${engine.outputs.map((d) => '${d.id}=${d.name}').join('; ')}');
  log('inputs: ${engine.inputs.map((d) => '${d.id}=${d.name}').join('; ')}');
  log('current: out ${engine.currentOutput}, in ${engine.currentInput}');
  final out = engine.currentOutput;
  check(
    out != null && engine.outputs.any((d) => d.id == out.id),
    'current output is in the list',
  );
  check(engine.outputs.first.id == 'default', 'outputs start with default');

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
  if (Platform.isAndroid) {
    await androidRoutes(engine);
  } else {
    await currentRoutes(engine);
  }
  await engine.close();
}

// ADR D15: each selection shows in currentOutput / currentInput when the
// select completes. iOS has no input route while capture is off.
Future<void> currentRoutes(AudioEngine engine) async {
  final changes = <RouteChange>[];
  final sub = engine.routeChanges.listen(changes.add);
  for (final d in engine.outputs.take(4)) {
    await engine.selectOutput(d.id);
    final c = engine.currentOutput;
    log('select output ${d.id}: $c');
    check(c?.id == d.id, 'current output is ${d.id}');
  }
  await engine.selectOutput('default');
  check(engine.currentOutput?.id == 'default', 'current output is default');
  if (!Platform.isIOS) {
    for (final d in engine.inputs.take(4)) {
      await engine.selectInput(d.id);
      final c = engine.currentInput;
      log('select input ${d.id}: $c');
      check(c?.id == d.id, 'current input is ${d.id}');
    }
    await engine.selectInput('default');
    check(engine.currentInput?.id == 'default', 'current input is default');
  }
  await sleep(100);
  await sub.cancel();
  log('route changes: ${changes.length}');
  if (engine.outputs.length > 1) {
    check(
      changes.any((c) => c.currentOutput?.id != 'default'),
      'a route change carries the selected output',
    );
  }
}

// ADR D6, D15 on Android: route selection at parity with flutter_webrtc.
// The earpiece and the Bluetooth microphone use communication mode; each
// move keeps the track position (I10). Each state holds 3 s, so
// `adb shell dumpsys audio` can show the mode.
Future<void> androidRoutes(AudioEngine engine) async {
  final outs = engine.outputs.map((d) => d.id).toList();
  final ins = engine.inputs.map((d) => d.id).toList();
  check(outs.first == 'default' && ins.first == 'default', 'default first');
  check(outs.contains('speaker'), 'speaker listed');

  final mp3 = await rootBundle.load('assets/speech.mp3');
  final track = await engine.createMp3Track(mp3.buffer.asUint8List());
  track
    ..setGain(0.15)
    ..play();
  await sleep(1000);
  await engine.startCapture(const CaptureConfig());
  var frames = 0;
  final sub = engine.capture.listen((f) => frames += f.pcm.length);

  final sw = Stopwatch()..start();
  Future<void> route(
    String what,
    Future<void> Function() select, {
    String? output,
    String? input,
  }) async {
    // The clip is 16.6 s: start each step early enough to hold 3 s.
    if (track.state.position > const Duration(seconds: 9)) {
      track.seek(const Duration(seconds: 1));
      await sleep(300);
    }
    final p0 = track.state.position;
    final f0 = frames;
    final t0 = sw.elapsedMilliseconds;
    try {
      await select();
    } catch (e) {
      check(false, '$what: $e');
      return;
    }
    final selectMs = sw.elapsedMilliseconds - t0;
    log(
      'ROUTE $what ($selectMs ms): out ${engine.currentOutput}, '
      'in ${engine.currentInput}',
    );
    if (output != null) {
      check(engine.currentOutput?.id == output, '$what: current output');
    }
    if (input != null) {
      check(engine.currentInput?.id == input, '$what: current input');
    }
    await sleep(3000);
    final played = (track.state.position - p0).inMilliseconds;
    final wall = sw.elapsedMilliseconds - t0;
    final captured = (frames - f0) ~/ 48;
    log(
      '$what: track moved $played ms in $wall ms (gap ${wall - played} ms), '
      'captured $captured ms, delay ${engine.outputDelay.inMilliseconds} ms',
    );
    check(track.state.status == TrackStatus.playing, '$what: still playing');
    check(played <= wall + 60, '$what: no skip');
    check(played >= wall - 1500, '$what: gap under 1.5 s');
    check(captured >= wall - 1500, '$what: capture continues');
  }

  if (outs.contains('earpiece')) {
    await route(
      'output earpiece',
      () => engine.selectOutput('earpiece'),
      output: 'earpiece',
    );
  }
  // adb shell setprop debug.tsnx.routes 1: the speaker in communication
  // mode, for devices without an earpiece (the emulator).
  if (outs.contains('debug-speaker-call')) {
    await route(
      'output debug-speaker-call',
      () => engine.selectOutput('debug-speaker-call'),
      output: 'debug-speaker-call',
    );
  }
  await route(
    'output speaker',
    () => engine.selectOutput('speaker'),
    output: 'speaker',
  );
  final mic = ins.firstWhere(
    (id) => id.startsWith('microphone'),
    orElse: () => '',
  );
  if (mic.isNotEmpty) {
    await route('input $mic', () => engine.selectInput(mic), input: mic);
  }
  if (ins.contains('bluetooth')) {
    await route(
      'input bluetooth',
      () => engine.selectInput('bluetooth'),
      input: 'bluetooth',
      output: 'bluetooth',
    );
    await route(
      'output speaker',
      () => engine.selectOutput('speaker'),
      output: 'speaker',
      input: 'default',
    );
  }
  await route(
    'output default',
    () => engine.selectOutput('default'),
    output: 'default',
  );
  // Communication mode ends after the last stream closes.
  if (outs.contains('debug-speaker-call')) {
    await route(
      'output debug-speaker-call',
      () => engine.selectOutput('debug-speaker-call'),
      output: 'debug-speaker-call',
    );
  }
  await sub.cancel();
  await engine.stopCapture();
  await track.dispose();
  log('ROUTE idle: streams close after the idle stop');
  await sleep(6000);
  log('ROUTE idle: done');
}
