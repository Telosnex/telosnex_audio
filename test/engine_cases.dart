// Engine cases that run on every platform: on the VM through FFI
// (engine_test.dart) and in Chrome through WebAssembly (web_engine_test.dart).
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:telosnex_audio/telosnex_audio.dart';

Int16List sine(int rate, double hz, Duration d, {int channels = 1}) {
  final n = rate * d.inMicroseconds ~/ 1000000;
  final out = Int16List(n * channels);
  for (var i = 0; i < n; i++) {
    final v = (12000 * math.sin(2 * math.pi * hz * i / rate)).round();
    for (var c = 0; c < channels; c++) {
      out[i * channels + c] = v;
    }
  }
  return out;
}

Future<void> settle() => Future<void>.delayed(const Duration(milliseconds: 5));

/// Renders blocks until [done] or [timeout]. Work outside the render (web
/// storage reads) needs real time between blocks.
Future<Int16List> renderUntil(
  AudioEngine engine,
  bool Function() done, {
  Duration timeout = const Duration(seconds: 5),
}) async {
  final out = <int>[];
  final sw = Stopwatch()..start();
  while (!done() && sw.elapsed < timeout) {
    out.addAll(engine.renderManual(1));
    await Future<void>.delayed(const Duration(milliseconds: 1));
  }
  return Int16List.fromList(out);
}

const _f0 = 300.0;
const _k = 150.0; // chirp: f(t) = f0 + k t

Int16List chirp(int rate, double seconds) {
  final out = Int16List((rate * seconds).round());
  for (var i = 0; i < out.length; i++) {
    final t = i / rate;
    out[i] = (12000 * math.sin(2 * math.pi * (_f0 * t + 0.5 * _k * t * t)))
        .round();
  }
  return out;
}

/// Median per-period frequency from interpolated rising zero crossings.
double localFreq(Int16List x, int start, int n, int rate) {
  final zc = <double>[];
  for (var i = start + 1; i < start + n && i < x.length; i++) {
    if (x[i - 1] < 0 && x[i] >= 0) {
      zc.add(i - 1 + -x[i - 1] / (x[i] - x[i - 1]));
    }
  }
  final f = <double>[
    for (var i = 1; i < zc.length; i++) rate / (zc[i] - zc[i - 1]),
  ];
  if (f.length < 3) return double.nan;
  f.sort();
  return f[f.length ~/ 2];
}

double pct(List<double> v, double p) {
  final s = [...v]..sort();
  return s[math.min(s.length - 1, (p * (s.length - 1)).round())];
}

int pattern(int frame) => (frame * 7) % 30011;

/// [open] gives an engine with `ManualDevice(channels: 1, delay: 40 ms)`.
/// [readFixture] reads a file from native_test/fixtures.
void engineCases(
  Future<AudioEngine> Function() open, {
  required Future<Uint8List> Function(String name) readFixture,
}) {
  late AudioEngine engine;

  setUp(() async {
    engine = await open();
  });

  tearDown(() async {
    await engine.close();
  });

  test('the engine is present on this host', () {
    expect(TelosnexAudio.isSupported, isTrue);
  });

  test('a streamed track plays, reports position, and ends once', () async {
    const fmt = PcmFormat(sampleRate: 24000, channels: 1);
    final t = engine.createTrack(fmt);
    final events = <TrackEvent>[];
    t.events.listen(events.add);
    expect(t.write(sine(24000, 440, const Duration(milliseconds: 500))), 12000);
    expect(t.state.status, TrackStatus.idle);
    t.play();
    final out = engine.renderManual(20);
    expect(out.length, 20 * 480);
    expect(out.reduce(math.max), greaterThan(5000));
    final s = t.state;
    expect(s.status, TrackStatus.playing);
    // The state after 20 blocks is sampled at the start of block 20
    // (190 ms); minus the 40 ms device delay, about 150 ms is heard.
    expect(s.position.inMilliseconds, inInclusiveRange(145, 155));
    expect(s.duration, isNull);
    t.endOfStream();
    engine.renderManual(60);
    await settle();
    expect(t.state.status, TrackStatus.ended);
    expect(t.state.position, const Duration(milliseconds: 500));
    expect(t.state.duration, const Duration(milliseconds: 500));
    expect(events.whereType<TrackStarted>(), hasLength(1));
    expect(events.whereType<TrackEnded>(), hasLength(1));
  });

  test('pause holds position; seek moves it; rate is reported', () async {
    const fmt = PcmFormat(sampleRate: 48000, channels: 2);
    final t = engine.createTrack(fmt);
    t.write(sine(48000, 300, const Duration(seconds: 3), channels: 2));
    t.endOfStream();
    t.play();
    engine.renderManual(30);
    t.pause();
    // Audio already in the device buffer is still heard for 40 ms.
    engine.renderManual(6);
    final held = t.state.position;
    final silent = engine.renderManual(10);
    expect(silent.every((v) => v == 0), isTrue);
    expect(t.state.position, held);
    expect(t.state.status, TrackStatus.paused);
    t.seek(const Duration(seconds: 2));
    engine.renderManual(1);
    expect(t.state.position, const Duration(seconds: 2));
    t.setRate(2.0);
    t.play();
    engine.renderManual(50);
    expect(t.state.rate, 2.0);
    // 500 ms at 2x is about 1 s of source, minus the 40 ms delay at 2x.
    expect(t.state.position.inMilliseconds, inInclusiveRange(2900, 3000));
  });

  test('flush returns the cut position and drops unplayed audio', () async {
    const fmt = PcmFormat(sampleRate: 24000, channels: 1);
    final t = engine.createTrack(fmt, retention: Retention.unplayed);
    t.write(sine(24000, 440, const Duration(seconds: 2)));
    t.play();
    engine.renderManual(25);
    final cut = t.flush();
    engine.renderManual(1);
    final position = await cut;
    expect(position.inMilliseconds, inInclusiveRange(250, 260));
    final after = engine.renderManual(10);
    expect(after.skip(480).every((v) => v == 0), isTrue);
  });

  test('an unplayed track refuses audio more than 30 s ahead', () {
    const fmt = PcmFormat(sampleRate: 8000, channels: 1);
    final t = engine.createTrack(fmt, retention: Retention.unplayed);
    final tenSeconds = Int16List(80000);
    t
      ..write(tenSeconds)
      ..write(tenSeconds)
      ..write(tenSeconds);
    expect(() => t.write(Int16List(8)), throwsA(isA<TrackFullError>()));
  });

  test('capture delivers post-APM frames at the requested rate', () async {
    final frames = <CaptureFrame>[];
    engine.capture.listen(frames.add);
    await engine.startCapture(const CaptureConfig(sampleRate: 24000));
    engine.renderManual(
      20,
      captureIn: sine(48000, 500, const Duration(milliseconds: 200)),
    );
    await settle();
    final samples = frames.fold<int>(0, (a, f) => a + f.pcm.length);
    expect(samples, 20 * 240);
    expect(frames.first.sampleRate, 24000);
    await engine.stopCapture();
  });

  test('eight tracks at most; dispose frees a slot', () async {
    const fmt = PcmFormat(sampleRate: 16000, channels: 1);
    final tracks = [for (var i = 0; i < 8; i++) engine.createTrack(fmt)];
    expect(() => engine.createTrack(fmt), throwsA(isA<AudioEngineException>()));
    await tracks.first.dispose();
    expect(() => tracks.first.play(), throwsStateError);
    expect(engine.createTrack(fmt), isA<Track>());
  });

  test('positionAt extrapolates while playing and caps at the buffer', () {
    const s = TrackState(
      status: TrackStatus.playing,
      position: Duration(seconds: 1),
      sampledAtNs: 0,
      rate: 2,
      buffered: Duration(milliseconds: 300),
      duration: null,
      writtenFrames: 0,
      positionFrames: 0,
    );
    expect(s.positionAt(100000000), const Duration(milliseconds: 1200));
    expect(s.positionAt(1000000000), const Duration(milliseconds: 1300));
  });

  // ADR I4: heard position against the chirp, through rate changes and a
  // seek. p95 at most 15 ms.
  test('position follows the chirp through rate changes and a seek', () async {
    const rate = 24000;
    final t = engine.createTrack(const PcmFormat(sampleRate: rate, channels: 1))
      ..write(chirp(rate, 16))
      ..endOfStream()
      ..play();
    final changes = <int, void Function()>{
      200: () => t.setRate(1.5),
      500: () => t.setRate(2.0),
      750: () => t.setRate(1.0),
      850: () => t.setRate(1.5),
      950: () => t.seek(const Duration(seconds: 3)),
      1150: () => t.setRate(2.0),
    };
    final out = <int>[];
    final positions = <double>[];
    var last = -1;
    var monotonic = true;
    for (var b = 0; b < 1400; b++) {
      changes[b]?.call();
      out.addAll(engine.renderManual(1));
      final s = t.state;
      // The device delay is 40 ms; the chirp is measured at the mixer.
      positions.add(s.positionFrames / rate + 0.040 * s.rate);
      if (s.status == TrackStatus.ended) break;
      if (last >= 0 && s.positionFrames < last && (b - 950).abs() > 6) {
        monotonic = false;
      }
      last = s.positionFrames;
      if (b % 50 == 0) await settle();
    }
    expect(monotonic, isTrue);
    final pcm = Int16List.fromList(out);
    final err = <double>[];
    for (var b = 6; b + 1 < positions.length; b++) {
      if ((b - 950).abs() < 10) continue;
      // Rate changes move the 40 ms of delay-compensation; skip them too.
      if (changes.keys.any((c) => b >= c && b < c + 6)) continue;
      final f = localFreq(pcm, b * 480 - 240, 960, 48000);
      final truth = (f - _f0) / _k;
      if (f.isNaN || truth < 0.05 || truth > 15.9) continue;
      final est = (positions[b] + positions[b + 1]) / 2;
      err.add((est - truth).abs() * 1000);
    }
    expect(err.length, greaterThan(700));
    expect(pct(err, .95), lessThanOrEqualTo(15.0));
  });

  // ADR I11: a seek outside the window plays the same samples as a seek
  // inside it. The backing store is a spill file (native) or the storage
  // worker (web).
  test('a far seek in a long streamed track plays the exact samples', () async {
    const rate = 48000;
    final t = engine.createTrack(
      const PcmFormat(sampleRate: rate, channels: 1),
    );
    for (var s = 0; s < 70; s++) {
      t.write(
        Int16List.fromList([
          for (var i = 0; i < rate; i++) pattern(s * rate + i),
        ]),
      );
    }
    t.endOfStream();
    engine.renderManual(5);
    await settle();
    t
      ..seek(const Duration(seconds: 60))
      ..play();
    final out = await renderUntil(
      engine,
      () =>
          t.state.status == TrackStatus.playing &&
          t.state.positionFrames > 60 * rate + 4800,
    );
    expect(t.state.status, TrackStatus.playing);
    // The first audible sample is frame 60 s; it fades in over 5 ms.
    final first = out.indexWhere((v) => v != 0);
    expect(first, greaterThanOrEqualTo(0));
    for (var i = 480; i < 2400; i++) {
      expect(out[first + i], pattern(60 * rate + i), reason: 'sample $i');
    }
  });

  test('MP3 track: format, duration, seek, and play to the end', () async {
    final bytes = await readFixture('shine_24k_mono_64k.mp3');
    final t = await engine.createMp3Track(bytes);
    expect(t.format, const PcmFormat(sampleRate: 24000, channels: 1));
    final d = t.state.duration!;
    expect(d.inMilliseconds, inInclusiveRange(16500, 16700));
    t
      ..seek(d - const Duration(seconds: 1))
      ..setRate(1.5)
      ..play();
    await renderUntil(engine, () => t.state.status == TrackStatus.ended);
    expect(t.state.status, TrackStatus.ended);
    expect(t.state.position, d);
  });

  test('an MP3 that does not decode is refused', () async {
    await expectLater(
      engine.createMp3Track(Uint8List.fromList(List.filled(4096, 7))),
      throwsA(isA<AudioEngineException>()),
    );
    // The refused open does not hold a slot.
    const fmt = PcmFormat(sampleRate: 16000, channels: 1);
    for (var i = 0; i < 8; i++) {
      engine.createTrack(fmt);
    }
  });
}
