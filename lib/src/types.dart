import 'dart:typed_data';

import 'package:meta/meta.dart';

/// How much of a track the engine keeps (ADR D11).
enum Retention {
  /// The whole track is seekable, up to 2 h. Streamed audio outside the
  /// memory window goes to a temporary file.
  all,

  /// Audio behind the playhead is freed. Writes more than 30 s ahead fail.
  unplayed,
}

enum TrackStatus { idle, playing, paused, starved, ended, failed }

@immutable
final class PcmFormat {
  const PcmFormat({required this.sampleRate, required this.channels})
    : assert(sampleRate % 100 == 0),
      assert(channels == 1 || channels == 2);

  final int sampleRate;
  final int channels;

  Duration durationOf(int frames) =>
      Duration(microseconds: frames * 1000000 ~/ sampleRate);
  int framesOf(Duration d) => d.inMicroseconds * sampleRate ~/ 1000000;

  @override
  bool operator ==(Object other) =>
      other is PcmFormat &&
      other.sampleRate == sampleRate &&
      other.channels == channels;
  @override
  int get hashCode => Object.hash(sampleRate, channels);
  @override
  String toString() => 'PcmFormat($sampleRate Hz, $channels ch)';
}

/// A snapshot of a track, published by the audio thread after each 10 ms
/// block. Reading it never waits (ADR D10).
@immutable
final class TrackState {
  const TrackState({
    required this.status,
    required this.position,
    required this.sampledAtNs,
    required this.rate,
    required this.buffered,
    required this.duration,
    required this.writtenFrames,
    required this.positionFrames,
  });

  final TrackStatus status;

  /// Heard position in source time: what comes out of the speaker at
  /// [sampledAtNs].
  final Duration position;

  /// When [position] was true, on the [AudioEngine.nowNs] clock.
  final int sampledAtNs;
  final double rate;

  /// Written, not yet heard.
  final Duration buffered;

  /// Null before [Track.endOfStream].
  final Duration? duration;
  final int writtenFrames;
  final int positionFrames;

  bool get isPlaying => status == TrackStatus.playing;

  /// [position] advanced to [nowNs] while playing.
  Duration positionAt(int nowNs) {
    if (status != TrackStatus.playing) return position;
    final advancedUs = (nowNs - sampledAtNs) * rate / 1000;
    var us = position.inMicroseconds + advancedUs.round();
    final d = duration;
    if (d != null && us > d.inMicroseconds) us = d.inMicroseconds;
    final cap = position + buffered;
    if (us > cap.inMicroseconds) us = cap.inMicroseconds;
    return Duration(microseconds: us);
  }

  @override
  String toString() =>
      'TrackState($status, $position, rate $rate, buffered $buffered, '
      'duration $duration)';
}

sealed class TrackEvent {
  const TrackEvent();
}

/// The first audio after play() reached the speaker.
final class TrackStarted extends TrackEvent {
  const TrackStarted();
}

/// A playing track ran out of written or loaded audio.
final class TrackStarved extends TrackEvent {
  const TrackStarved();
}

final class TrackResumed extends TrackEvent {
  const TrackResumed();
}

/// The last frame after endOfStream() reached the speaker (ADR I7).
final class TrackEnded extends TrackEvent {
  const TrackEnded();
}

final class TrackFailed extends TrackEvent {
  const TrackFailed(this.reason);
  final String reason;
}

/// A `.unplayed` track is more than 30 s ahead, or a `.all` track is past
/// 2 h.
final class TrackFullError extends StateError {
  TrackFullError(super.message);
}

final class AudioEngineException implements Exception {
  const AudioEngineException(this.code, this.message);
  final int code;
  final String message;
  @override
  String toString() => 'AudioEngineException($code): $message';
}

/// The audio device behind an engine.
sealed class EngineDevice {
  const EngineDevice();
}

/// The platform's audio device.
final class PlatformDevice extends EngineDevice {
  const PlatformDevice({this.appleVoiceProcessing = false});

  /// macOS: AVAudioEngine with Apple voice processing instead of CoreAudio
  /// with WebRTC's AEC3. For comparisons; AEC3 is the default.
  final bool appleVoiceProcessing;
}

/// No device: tests drive the engine with [AudioEngine.renderManual].
final class ManualDevice extends EngineDevice {
  const ManualDevice({this.channels = 2, this.delay = Duration.zero});
  final int channels;

  /// Simulated output delay between the mixer and the speaker.
  final Duration delay;
}

enum ClockCorrection { off, observe, control }

@immutable
final class AudioProcessingConfig {
  const AudioProcessingConfig({
    this.echoCancellation = true,
    this.noiseSuppression = true,
    this.autoGain = true,
    this.clockCorrection = ClockCorrection.off,
  });
  final bool echoCancellation;
  final bool noiseSuppression;
  final bool autoGain;
  final ClockCorrection clockCorrection;
}

@immutable
final class CaptureConfig {
  const CaptureConfig({this.sampleRate = 48000});

  /// Post-APM output rate. A multiple of 100, 8000-48000.
  final int sampleRate;
}

/// Post-APM microphone audio, mono PCM16.
@immutable
final class CaptureFrame {
  const CaptureFrame({
    required this.pcm,
    required this.sampleRate,
    required this.capturedAtNs,
  });
  final Int16List pcm;
  final int sampleRate;

  /// When the first sample was captured, on the [AudioEngine.nowNs] clock.
  final int capturedAtNs;
  int get channels => 1;
}

@immutable
final class AudioDevice {
  const AudioDevice({required this.id, required this.name});
  final String id;
  final String name;
  @override
  bool operator ==(Object other) =>
      other is AudioDevice && other.id == id && other.name == name;
  @override
  int get hashCode => Object.hash(id, name);
  @override
  String toString() => 'AudioDevice($name)';
}

@immutable
final class RouteChange {
  const RouteChange({required this.inputs, required this.outputs});
  final List<AudioDevice> inputs;
  final List<AudioDevice> outputs;
}

/// One playback source in the engine mixer (ADR D7).
abstract interface class Track {
  int get id;
  PcmFormat get format;

  /// Appends interleaved PCM16. Returns the frames written. Throws
  /// [TrackFullError] when the track cannot take more audio.
  int write(Int16List interleaved);
  void endOfStream();
  void play();
  void pause();
  void seek(Duration position);
  void setRate(double rate);
  void setGain(double gain, {Duration ramp = const Duration(milliseconds: 10)});

  /// Drops audio not yet played. Completes when the audio thread applied it,
  /// with the position where the audio was cut.
  Future<Duration> flush();
  TrackState get state;
  Stream<TrackEvent> get events;
  Future<void> dispose();
}

/// The engine: device, echo cancellation, mixer, tracks, and capture.
abstract interface class AudioEngine {
  /// The engine clock in nanoseconds; the same clock as
  /// [TrackState.sampledAtNs] and [CaptureFrame.capturedAtNs].
  int nowNs();

  Track createTrack(PcmFormat format, {Retention retention = Retention.all});
  Future<Track> createMp3Track(Uint8List mp3);

  Future<void> startCapture(CaptureConfig config);
  Future<void> stopCapture();
  Stream<CaptureFrame> get capture;
  List<AudioDevice> get inputs;
  List<AudioDevice> get outputs;
  Future<void> selectInput(String deviceId);
  Future<void> selectOutput(String deviceId);
  Stream<RouteChange> get routeChanges;

  /// Output delay reported by the device.
  Duration get outputDelay;

  /// [ManualDevice] only: renders [blocks] 10 ms blocks and returns them,
  /// 48 kHz interleaved. [captureIn] holds the same number of 48 kHz mono
  /// blocks for the capture path.
  Int16List renderManual(int blocks, {Int16List? captureIn});

  Future<void> close();
}
