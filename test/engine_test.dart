// Dart API tests with the manual device (ADR workplan step 5 gate).
import 'dart:io';
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

void main() {
  late AudioEngine engine;
  late Directory spill;

  setUp(() async {
    spill = Directory.systemTemp.createTempSync('tsnx_dart_test');
    engine = await TelosnexAudio.open(
      device: const ManualDevice(
        channels: 1,
        delay: Duration(milliseconds: 40),
      ),
      spillDir: spill.path,
    );
  });

  tearDown(() async {
    await engine.close();
    if (spill.existsSync()) spill.deleteSync(recursive: true);
  });

  test('the native engine is present on this host', () {
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

  test('MP3 track: format, duration, seek, and play to the end', () async {
    final bytes = File(
      'native_test/fixtures/shine_24k_mono_64k.mp3',
    ).readAsBytesSync();
    final t = await engine.createMp3Track(bytes);
    expect(t.format, const PcmFormat(sampleRate: 24000, channels: 1));
    final d = t.state.duration!;
    expect(d.inMilliseconds, inInclusiveRange(16500, 16700));
    t.seek(d - const Duration(seconds: 1));
    t.setRate(1.5);
    t.play();
    engine.renderManual(100);
    await settle();
    expect(t.state.status, TrackStatus.ended);
    expect(t.state.position, d);
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
}
