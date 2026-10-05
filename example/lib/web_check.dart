// Real-time check of the web engine (ADR workplan step 9 gate), in a
// browser AudioWorklet:
//
//   flutter build web -t lib/web_check.dart
//   dart ../tool/web_check.dart --browser chrome   (or safari)
//
// A streamed 24 kHz track holds a 16 s chirp, repeated to 80 s. It plays
// with rate changes and two seeks outside the memory window, which the
// storage worker serves. A tap after the engine records the output. Each
// posted position is checked against the chirp frequency at that frame
// (ADR I4: p95 at most 15 ms).
//
// Lines start with TSNX_WEB; the last one is PASS or FAIL. With
// ?report=<url> the page also POSTs the lines there.
import 'dart:async';
import 'dart:js_interop';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:telosnex_audio/telosnex_audio.dart';
import 'package:telosnex_audio/web_testing.dart';
import 'package:web/web.dart' as web;

final lines = <String>[];
void log(String s) {
  lines.add(s);
  debugPrint('TSNX_WEB: $s');
}

final failures = <String>[];
void check(bool ok, String what) {
  log('${ok ? 'ok  ' : 'FAIL'} $what');
  if (!ok) failures.add(what);
}

const rate = 24000;
const f0 = 300.0;
const k = 150.0; // f(t) = f0 + k t, t in 0..16 s, repeated
const period = 16.0;

Int16List chirp() {
  final out = Int16List((rate * period).round());
  for (var i = 0; i < out.length; i++) {
    final t = i / rate;
    out[i] = (12000 * math.sin(2 * math.pi * (f0 * t + 0.5 * k * t * t)))
        .round();
  }
  return out;
}

double localFreq(Float32List x, int start, int n, int sr) {
  final zc = <double>[];
  for (var i = math.max(1, start + 1); i < start + n && i < x.length; i++) {
    if (x[i - 1] < 0 && x[i] >= 0) {
      zc.add(i - 1 + -x[i - 1] / (x[i] - x[i - 1]));
    }
  }
  final f = <double>[
    for (var i = 1; i < zc.length; i++) sr / (zc[i] - zc[i - 1]),
  ];
  if (f.length < 3) return double.nan;
  f.sort();
  return f[f.length ~/ 2];
}

double pct(List<double> v, double p) {
  final s = [...v]..sort();
  return s[math.min(s.length - 1, (p * (s.length - 1)).round())];
}

void main() => runApp(const MaterialApp(home: CheckPage()));

class CheckPage extends StatefulWidget {
  const CheckPage({super.key});
  @override
  State<CheckPage> createState() => _CheckPageState();
}

class _CheckPageState extends State<CheckPage> {
  AudioEngine? engine;
  String status = 'opening';

  @override
  void initState() {
    super.initState();
    unawaited(open());
  }

  Future<void> open() async {
    final e = await TelosnexAudio.open();
    engine = e;
    // Chrome with --autoplay-policy=no-user-gesture-required runs at once.
    for (var i = 0; i < 20 && debugContextState(e) != 'running'; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 50));
    }
    if (debugContextState(e) == 'running') {
      await start();
    } else {
      setState(() => status = 'click Start (the browser needs a gesture)');
    }
  }

  Future<void> start() async {
    final e = engine!;
    setState(() => status = 'running');
    try {
      await debugResume(e);
      await run(e);
    } catch (err, st) {
      failures.add('$err');
      log('exception $err\n$st');
    }
    log(failures.isEmpty ? 'PASS' : 'FAIL ${failures.length}');
    web.document.title = failures.isEmpty ? 'TSNX PASS' : 'TSNX FAIL';
    setState(() => status = failures.isEmpty ? 'PASS' : 'FAIL');
    await report();
  }

  @override
  Widget build(BuildContext context) => Scaffold(
    body: Center(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Text('web check: $status'),
          if (status.startsWith('click'))
            ElevatedButton(onPressed: start, child: const Text('Start')),
        ],
      ),
    ),
  );
}

Future<void> report() async {
  final url = Uri.base.queryParameters['report'];
  if (url == null) return;
  await web.window
      .fetch(
        url.toJS,
        web.RequestInit(method: 'POST', body: lines.join('\n').toJS),
      )
      .toDart;
}

Future<void> run(AudioEngine engine) async {
  log('user agent: ${web.window.navigator.userAgent}');
  debugSetOutputDelay(engine, Duration.zero);
  final tap = await debugTapOutput(engine);
  log('context rate ${tap.sampleRate}');

  final t = engine.createTrack(const PcmFormat(sampleRate: rate, channels: 1));
  final starved = <int>[];
  t.events.listen((ev) {
    if (ev is TrackStarved) starved.add(engine.nowNs());
  });
  t.setGain(0.05, ramp: Duration.zero); // quiet on real speakers

  // Stream 80 s at 20x real time, like fast TTS.
  final c = chirp();
  var written = 0;
  final writer = Timer.periodic(const Duration(milliseconds: 50), (timer) {
    final i = written % 16;
    t.write(Int16List.sublistView(c, i * rate, (i + 1) * rate));
    written++;
    if (written == 80) {
      t.endOfStream();
      timer.cancel();
    }
  });
  await Future<void>.delayed(const Duration(milliseconds: 300));

  tap.start();
  final samples = <(int, int, TrackStatus)>[]; // sampledAtNs, frames
  final seeks = <int>[];
  var lastAt = -1;
  final poll = Timer.periodic(const Duration(milliseconds: 5), (_) {
    final s = t.state;
    if (s.sampledAtNs == lastAt) return;
    lastAt = s.sampledAtNs;
    samples.add((s.sampledAtNs, s.positionFrames, s.status));
  });
  final sw = Stopwatch()..start();
  t.play();
  Future<void> at(int ms, void Function() f) async {
    final wait = ms - sw.elapsedMilliseconds;
    if (wait > 0) await Future<void>.delayed(Duration(milliseconds: wait));
    f();
  }

  await at(3000, () => t.setRate(1.5));
  await at(5000, () => t.setRate(2.0));
  await at(7000, () {
    seeks.add(engine.nowNs());
    t.seek(const Duration(seconds: 60));
  });
  await at(9000, () => t.setRate(1.0));
  await at(11000, () {
    seeks.add(engine.nowNs());
    t.seek(const Duration(seconds: 5));
  });
  await at(13000, () => t.setRate(1.5));
  await at(15000, () {});
  poll.cancel();
  writer.cancel();
  t.pause();
  await Future<void>.delayed(const Duration(milliseconds: 300));
  tap.stop();
  await Future<void>.delayed(const Duration(milliseconds: 200));
  await t.dispose();

  check(written == 80, 'the stream was written (80 s, got $written s)');
  // The tap's frame clock is the engine clock.
  final sr = tap.sampleRate;
  if (tap.pieces.isEmpty) {
    check(false, 'tap recorded output');
    return;
  }
  final first = tap.pieces.first.$1;
  final total = tap.pieces.fold<int>(0, (a, p) => a + p.$2.length);
  final out = Float32List(total);
  var off = 0;
  for (final (frame, pcm) in tap.pieces) {
    if (frame - first != off) {
      check(false, 'tap pieces are contiguous');
      return;
    }
    out.setAll(off, pcm);
    off += pcm.length;
  }
  log('tap: ${(total / sr).toStringAsFixed(2)} s, ${samples.length} states');

  // ADR I4.
  bool nearSeek(int ns) =>
      seeks.any((s) => ns >= s - 20000000 && ns < s + 600000000);
  final err = <double>[];
  var monotonic = true;
  var lastPos = -1;
  var lastNs = 0;
  for (final (ns, pos, status) in samples) {
    if (lastPos >= 0 && pos < lastPos && !nearSeek(ns)) monotonic = false;
    lastPos = pos;
    if (status != TrackStatus.playing || nearSeek(ns)) continue;
    if (ns - lastNs < 9000000) continue; // one sample per block
    lastNs = ns;
    final frame = (ns * sr / 1e9).round() - first;
    if (frame < sr ~/ 50 || frame + sr ~/ 50 > out.length) continue;
    final f = localFreq(out, frame - sr ~/ 100, sr ~/ 50, sr);
    if (f.isNaN) continue;
    final truth = (f - f0) / k;
    if (truth < 0.1 || truth > period - 0.1) continue;
    final reported = (pos / rate) % period;
    var d = (reported - truth).abs();
    d = math.min(d, period - d);
    err.add(d * 1000);
  }
  check(monotonic, 'position does not decrease between seeks');
  check(err.length > 500, 'enough checked positions (${err.length})');
  if (err.isNotEmpty) {
    log(
      'position error ms: p50 ${pct(err, .5).toStringAsFixed(1)} '
      'p95 ${pct(err, .95).toStringAsFixed(1)} '
      'max ${pct(err, 1).toStringAsFixed(1)}',
    );
    check(pct(err, .95) <= 15, 'I4: p95 position error at most 15 ms');
  }

  // Far seeks: playing again soon, from the right place.
  for (final (i, s) in seeks.indexed) {
    final target = i == 0 ? 60 : 5;
    final resumed = samples.where(
      (x) =>
          x.$1 > s &&
          x.$3 == TrackStatus.playing &&
          (x.$2 / rate - target).abs() < 1,
    );
    final ms = resumed.isEmpty ? -1 : (resumed.first.$1 - s) / 1e6;
    log(
      'seek to $target s: playing at the target after '
      '${ms.toStringAsFixed(0)} ms',
    );
    check(ms >= 0 && ms < 500, 'far seek to $target s plays within 500 ms');
  }
  final stray = starved.where((ns) => !nearSeek(ns)).length;
  check(stray == 0, 'no starvation outside seeks ($stray)');

  await idleStop(engine);
  if (!Uri.base.queryParameters.containsKey('nocapture')) {
    await captureCheck(engine);
  }
}

// The context suspends after 3 s with nothing to play, and play() resumes
// it (the native idle stop).
Future<void> idleStop(AudioEngine engine) async {
  await Future<void>.delayed(const Duration(milliseconds: 4000));
  check(
    debugContextState(engine) == 'suspended',
    'idle: the context suspended (${debugContextState(engine)})',
  );
  final t = engine.createTrack(const PcmFormat(sampleRate: 24000, channels: 1));
  final started = Completer<void>();
  t.events.listen((e) {
    if (e is TrackStarted && !started.isCompleted) started.complete();
  });
  t
    ..write(Int16List(24000))
    ..endOfStream()
    ..play();
  final sw = Stopwatch()..start();
  await started.future.timeout(const Duration(seconds: 2), onTimeout: () {});
  check(
    started.isCompleted && debugContextState(engine) == 'running',
    'play() resumes the context (started after ${sw.elapsedMilliseconds} ms)',
  );
  await t.dispose();
}

// D15 on web: getUserMedia with the browser's echo canceller, resampled to
// the asked rate.
Future<void> captureCheck(AudioEngine engine) async {
  final frames = <CaptureFrame>[];
  final sub = engine.capture.listen(frames.add);
  try {
    await engine.startCapture(const CaptureConfig(sampleRate: 24000));
  } catch (e) {
    check(false, 'startCapture: $e');
    await sub.cancel();
    return;
  }
  await Future<void>.delayed(const Duration(seconds: 2));
  await engine.stopCapture();
  await sub.cancel();
  final samples = frames.fold<int>(0, (a, f) => a + f.pcm.length);
  final ms = samples / 24;
  log(
    'capture: ${frames.length} frames, ${ms.round()} ms, '
    'inputs ${engine.inputs.map((d) => d.name).join(', ')}',
  );
  check(frames.every((f) => f.sampleRate == 24000), 'capture rate 24 kHz');
  check(ms > 1500 && ms < 2300, 'capture delivers about 2 s in 2 s');
  var ordered = true;
  for (var i = 1; i < frames.length; i++) {
    final d = frames[i].capturedAtNs - frames[i - 1].capturedAtNs;
    if (d < 9000000 || d > 11000000) ordered = false;
  }
  check(ordered, 'capture timestamps step by 10 ms');
}
