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
  // Each line also goes to the runner as it happens, so a run that never
  // reports shows how far it got.
  final url = Uri.base.queryParameters['report'];
  if (url != null) {
    unawaited(
      web.window
          .fetch(
            '$url/line'.toJS,
            web.RequestInit(method: 'POST', body: s.toJS),
          )
          .toDart
          .then((_) {}, onError: (Object _) {}),
    );
  }
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
    // Browsers that allow autoplay (the CI flags and prefs) resume without a
    // gesture; the others leave resume() pending until a click. Waiting for
    // the context to start by itself was not enough: on CI, headless Chrome
    // (Wasm build) and Firefox stayed suspended for 10 s (2026-10-27).
    log('opened; context ${debugContextState(e)}');
    try {
      await debugResume(e).timeout(const Duration(seconds: 10));
    } on TimeoutException {
      // Pending: the browser needs a gesture.
    }
    if (debugContextState(e) == 'running') {
      await start();
    } else {
      log(
        'context ${debugContextState(e)} after resume(): waiting for a click',
      );
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
  log('dart2wasm: ${const bool.fromEnvironment('dart.tool.dart2wasm')}');
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
  // Where an event lies: ms after play, and after the last seek.
  String when(int ns) {
    final since = seeks.where((x) => x <= ns).toList();
    final play = samples.isEmpty ? ns : samples.first.$1;
    return '${((ns - play) / 1e6).round()} ms after play'
        '${since.isEmpty ? '' : ', ${((ns - since.last) / 1e6).round()} ms after seek ${since.length}'}';
  }

  for (final (ns, pos, status) in samples) {
    if (lastPos >= 0 && pos < lastPos && !nearSeek(ns)) {
      monotonic = false;
      log(
        'position decreased: $lastPos -> $pos frames (${status.name}) '
        'at ${when(ns)}',
      );
    }
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
  // The page polls every 5 ms; headless Firefox on a loaded CI runner runs
  // that timer less often (471, 694, 707 checked positions in three runs on
  // 2026-10-27; Chrome and Safari about 950). With 300, 15 samples still lie
  // above the p95.
  check(err.length > 300, 'enough checked positions (${err.length})');
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
  final strays = starved.where((ns) => !nearSeek(ns)).toList();
  for (final ns in strays) {
    log('starved at ${when(ns)}');
  }
  final stray = strays.length;
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
  // Firefox can render the first quantum (TrackStarted) before it flips
  // AudioContext.state to 'running', so give the state a moment to follow.
  final stateDeadline = Stopwatch()..start();
  while (started.isCompleted &&
      debugContextState(engine) != 'running' &&
      stateDeadline.elapsedMilliseconds < 500) {
    await Future<void>.delayed(const Duration(milliseconds: 20));
  }
  check(
    started.isCompleted && debugContextState(engine) == 'running',
    'play() resumes the context (started after ${sw.elapsedMilliseconds} ms, '
    'state ${debugContextState(engine)})',
  );
  await t.dispose();
}

// D15 on web: getUserMedia with the browser's echo canceller, resampled to
// the asked rate.
Future<void> captureCheck(AudioEngine engine) async {
  final frames = <CaptureFrame>[];
  final clock = Stopwatch()..start();
  Duration? first;
  final sub = engine.capture.listen((f) {
    first ??= clock.elapsed;
    frames.add(f);
  });
  try {
    await engine.startCapture(const CaptureConfig(sampleRate: 24000));
  } catch (e) {
    check(false, 'startCapture: $e');
    await sub.cancel();
    return;
  }
  await Future<void>.delayed(const Duration(seconds: 2));
  final stopNs = engine.nowNs();
  await engine.stopCapture();
  await sub.cancel();
  final samples = frames.fold<int>(0, (a, f) => a + f.pcm.length);
  final ms = samples / 24;
  // Delivery is measured on the audio clock (nowNs and capturedAtNs), not
  // the wall clock: headless Chrome's context clock ran at 0.7x wall time
  // on a loaded CI runner (1330 ms of capture in 1879 wall ms, timestamps
  // still 10 ms apart; 2026-10-27), which the engine does not control.
  final startMs = first?.inMilliseconds ?? -1;
  final spanMs = frames.isEmpty
      ? 0.0
      : (stopNs - frames.first.capturedAtNs) / 1e6;
  log(
    'capture: ${frames.length} frames, ${ms.round()} ms; audio clock '
    '${spanMs.round()} ms from the first frame to stop; first frame '
    'after $startMs wall ms; '
    'inputs ${engine.inputs.map((d) => d.name).join(', ')}',
  );
  check(frames.every((f) => f.sampleRate == 24000), 'capture rate 24 kHz');
  check(startMs >= 0 && startMs < 1200, 'capture starts within 1.2 s');
  // Slack: output latency in nowNs and the blocks still in flight at stop.
  check(
    spanMs > 500 && (ms - spanMs).abs() < spanMs * 0.1 + 150,
    'capture delivers the audio clock\'s time',
  );
  var ordered = true;
  for (var i = 1; i < frames.length; i++) {
    final d = frames[i].capturedAtNs - frames[i - 1].capturedAtNs;
    if (d < 9000000 || d > 11000000) ordered = false;
  }
  check(ordered, 'capture timestamps step by 10 ms');
  await deviceCheck(engine);
}

// D15 on web: the current devices follow the selection.
Future<void> deviceCheck(AudioEngine engine) async {
  log(
    'devices: out ${engine.outputs.map((d) => d.id).join(', ')}; '
    'current out ${engine.currentOutput}, in ${engine.currentInput}',
  );
  check(engine.outputs.first.id == 'default', 'outputs start with default');
  check(engine.currentOutput?.id == 'default', 'current output is default');
  if (engine.inputs.isNotEmpty) {
    check(engine.inputs.first.id == 'default', 'inputs start with default');
    check(engine.currentInput?.id == 'default', 'current input is default');
  }
  for (final d in engine.outputs.take(3)) {
    try {
      await engine.selectOutput(d.id);
    } catch (e) {
      check(false, 'selectOutput ${d.id}: $e');
      continue;
    }
    check(engine.currentOutput?.id == d.id, 'current output is ${d.id}');
  }
  await engine.selectOutput('default');
  check(
    engine.currentOutput?.id == 'default',
    'current output is default again',
  );
}
