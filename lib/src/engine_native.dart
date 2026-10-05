import 'dart:async';
import 'dart:developer' as developer;
import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'ffi.dart';
import 'types.dart';

bool? _supported;

bool get engineSupported {
  return _supported ??= () {
    try {
      tsnxVersion();
      return true;
    } on ArgumentError {
      return false;
    }
  }();
}

Future<AudioEngine> openEngine({
  required EngineDevice device,
  required AudioProcessingConfig processing,
  String? spillDir,
}) async {
  if (!engineSupported) {
    throw UnsupportedError(
      'telosnex_audio has no native engine for this build',
    );
  }
  return _NativeEngine.open(device, processing, spillDir);
}

const _kTrackStarted = 1;
const _kTrackStarved = 2;
const _kTrackResumed = 3;
const _kTrackEnded = 4;
const _kTrackFailed = 5;
const _kTrackFlushed = 6;
const _kCaptureReady = 100;
const _kRequestDone = 101;
const _kDevicesChanged = 102;
const _kEngineError = 103;

const _kWriteFull = -101;
const _kWriteEnded = -102;
const _kWriteIoError = -103;
const _kCaptureBatch = 64;

String _errorText(int code) => switch (code) {
  -1 => 'invalid argument',
  -2 => 'no free track (8 maximum)',
  -3 => 'no such track',
  -4 => 'device error',
  -5 => 'unsupported format',
  -6 => 'command queue full',
  _ => 'error $code',
};

void _check(int code, String what) {
  if (code < 0) throw AudioEngineException(code, '$what: ${_errorText(code)}');
}

final class _NativeEngine implements AudioEngine {
  _NativeEngine._(this._e, this._listener, this._manual);

  static Future<_NativeEngine> open(
    EngineDevice device,
    AudioProcessingConfig processing,
    String? spillDir,
  ) async {
    late final _NativeEngine engine;
    final listener =
        NativeCallable<Void Function(Int32, Int32, Int64)>.listener(
          (int kind, int id, int value) => engine._onNotify(kind, id, value),
        );
    final dir = spillDir ?? '${Directory.systemTemp.path}/telosnex_audio';
    final config = calloc<TsnxEngineConfig>();
    final out = calloc<Pointer<TsnxEngine>>();
    final dirPtr = dir.toNativeUtf8();
    try {
      final ref = config.ref
        ..echo_cancellation = processing.echoCancellation ? 1 : 0
        ..noise_suppression = processing.noiseSuppression ? 1 : 0
        ..auto_gain = processing.autoGain ? 1 : 0
        ..clock_correction = processing.clockCorrection.index
        ..spill_dir = dirPtr
        ..notify = listener.nativeFunction;
      switch (device) {
        case ManualDevice(:final channels, :final delay):
          ref
            ..manual_device = 1
            ..manual_output_channels = channels
            ..manual_delay_ms = delay.inMilliseconds;
        case PlatformDevice(:final appleVoiceProcessing, :final linuxBackend):
          ref
            ..platform_voice_processing = appleVoiceProcessing ? 1 : 0
            ..linux_audio_backend = linuxBackend.index;
      }
      final rc = tsnxEngineOpen(config, out);
      if (rc != 0) {
        listener.close();
        throw AudioEngineException(rc, 'open failed: ${_errorText(rc)}');
      }
      engine = _NativeEngine._(
        out.value,
        listener,
        device is ManualDevice ? device : null,
      );
      return engine;
    } finally {
      calloc
        ..free(config)
        ..free(out)
        ..free(dirPtr);
    }
  }

  final Pointer<TsnxEngine> _e;
  final NativeCallable<Void Function(Int32, Int32, Int64)> _listener;
  final ManualDevice? _manual;
  final Map<int, _NativeTrack> _tracks = {};
  final Map<int, Completer<void>> _requests = {};
  int _nextRequest = 1;
  bool _closed = false;

  final Pointer<TsnxTrackState> _state = calloc<TsnxTrackState>();
  final Pointer<TsnxCaptureBlock> _blocks = calloc<TsnxCaptureBlock>(
    _kCaptureBatch,
  );
  final StreamController<CaptureFrame> _capture =
      StreamController<CaptureFrame>.broadcast(sync: true);
  final StreamController<RouteChange> _routes =
      StreamController<RouteChange>.broadcast();

  void _checkOpen() {
    if (_closed) throw StateError('AudioEngine is closed');
  }

  @override
  int nowNs() => tsnxEngineNowNs(_e);

  @override
  Duration get outputDelay =>
      Duration(milliseconds: tsnxEngineOutputDelayMs(_e));

  @override
  Track createTrack(PcmFormat format, {Retention retention = Retention.all}) {
    _checkOpen();
    final out = calloc<Int32>();
    try {
      _check(
        tsnxTrackCreate(
          _e,
          format.sampleRate,
          format.channels,
          retention.index,
          out,
        ),
        'createTrack',
      );
      return _register(out.value, format);
    } finally {
      calloc.free(out);
    }
  }

  @override
  Future<Track> createMp3Track(Uint8List mp3) async {
    _checkOpen();
    final data = malloc<Uint8>(mp3.length);
    final out = calloc<Int32>();
    final rate = calloc<Int32>();
    final channels = calloc<Int32>();
    try {
      data.asTypedList(mp3.length).setAll(0, mp3);
      _check(tsnxTrackCreateMp3(_e, data, mp3.length, out), 'createMp3Track');
      final id = out.value;
      _check(tsnxTrackFormat(_e, id, rate, channels), 'createMp3Track');
      return _register(
        id,
        PcmFormat(sampleRate: rate.value, channels: channels.value),
      );
    } finally {
      malloc.free(data);
      calloc
        ..free(out)
        ..free(rate)
        ..free(channels);
    }
  }

  _NativeTrack _register(int id, PcmFormat format) {
    final t = _NativeTrack(this, id, format);
    _tracks[id] = t;
    return t;
  }

  TrackState _readState(_NativeTrack t) {
    final rc = tsnxTrackGetState(_e, t.id, _state);
    if (rc != 0) {
      return TrackState(
        status: TrackStatus.failed,
        position: Duration.zero,
        sampledAtNs: nowNs(),
        rate: 1,
        buffered: Duration.zero,
        duration: null,
        writtenFrames: 0,
        positionFrames: 0,
      );
    }
    final s = _state.ref;
    final f = t.format;
    return TrackState(
      status: TrackStatus.values[s.status],
      position: f.durationOf(s.position),
      sampledAtNs: s.sampled_at_ns,
      rate: s.rate,
      buffered: f.durationOf(
        s.written - s.position < 0 ? 0 : s.written - s.position,
      ),
      duration: s.duration < 0 ? null : f.durationOf(s.duration),
      writtenFrames: s.written,
      positionFrames: s.position,
    );
  }

  Future<void> _request(int Function(int requestId) call, String what) {
    _checkOpen();
    final id = _nextRequest++;
    final c = Completer<void>();
    _requests[id] = c;
    final rc = call(id);
    if (rc != 0) {
      _requests.remove(id);
      return Future.error(AudioEngineException(rc, '$what: ${_errorText(rc)}'));
    }
    return c.future.catchError((Object e) {
      if (e is AudioEngineException) {
        throw AudioEngineException(e.code, '$what: ${e.message}');
      }
      throw e;
    });
  }

  @override
  Future<void> startCapture(CaptureConfig config) => _request(
    (id) => tsnxCaptureStart(_e, config.sampleRate, id),
    'startCapture',
  );

  @override
  Future<void> stopCapture() =>
      _request((id) => tsnxCaptureStop(_e, id), 'stopCapture');

  @override
  Stream<CaptureFrame> get capture => _capture.stream;

  List<AudioDevice> _devices(int kind) {
    _checkOpen();
    final n = tsnxDeviceCount(_e, kind);
    final id = calloc<Uint8>(512).cast<Utf8>();
    final name = calloc<Uint8>(512).cast<Utf8>();
    try {
      return [
        for (var i = 0; i < n; i++)
          if (tsnxDeviceGet(_e, kind, i, id, 512, name, 512) == 0)
            AudioDevice(id: id.toDartString(), name: name.toDartString()),
      ];
    } finally {
      calloc
        ..free(id)
        ..free(name);
    }
  }

  CurrentDevice? _current(int kind) {
    _checkOpen();
    final id = calloc<Uint8>(512).cast<Utf8>();
    final name = calloc<Uint8>(512).cast<Utf8>();
    final deviceKind = calloc<Int32>();
    try {
      if (tsnxDeviceCurrent(_e, kind, id, 512, name, 512, deviceKind) != 0) {
        return null;
      }
      final k = deviceKind.value;
      return CurrentDevice(
        id: id.toDartString(),
        name: name.toDartString(),
        kind: k >= 0 && k < AudioDeviceKind.values.length
            ? AudioDeviceKind.values[k]
            : AudioDeviceKind.other,
      );
    } finally {
      calloc
        ..free(id)
        ..free(name)
        ..free(deviceKind);
    }
  }

  @override
  CurrentDevice? get currentInput => _current(1);

  @override
  CurrentDevice? get currentOutput => _current(0);

  @override
  List<AudioDevice> get inputs => _devices(1);

  @override
  List<AudioDevice> get outputs => _devices(0);

  Future<void> _select(int kind, String deviceId, String what) {
    final p = deviceId.toNativeUtf8();
    return _request(
      (id) => tsnxDeviceSelect(_e, kind, p, id),
      what,
    ).whenComplete(() => calloc.free(p));
  }

  @override
  Future<void> selectInput(String deviceId) =>
      _select(1, deviceId, 'selectInput');

  @override
  Future<void> selectOutput(String deviceId) =>
      _select(0, deviceId, 'selectOutput');

  @override
  Stream<RouteChange> get routeChanges => _routes.stream;

  @override
  Int16List renderManual(int blocks, {Int16List? captureIn}) {
    _checkOpen();
    final manual = _manual;
    if (manual == null) throw StateError('renderManual needs a ManualDevice');
    final samples = blocks * 480 * manual.channels;
    final out = calloc<Int16>(samples);
    final cap = captureIn == null ? nullptr : calloc<Int16>(blocks * 480);
    try {
      if (captureIn != null) {
        if (captureIn.length != blocks * 480) {
          throw ArgumentError.value(
            captureIn.length,
            'captureIn',
            'must hold $blocks blocks of 480 mono samples',
          );
        }
        cap.asTypedList(blocks * 480).setAll(0, captureIn);
      }
      _check(tsnxEngineManualRender(_e, blocks, out, cap), 'renderManual');
      return Int16List.fromList(out.asTypedList(samples));
    } finally {
      calloc.free(out);
      if (cap != nullptr) calloc.free(cap);
    }
  }

  void _drainCapture() {
    final frames = <Int16List>[];
    var first = -1;
    var rate = 0;
    for (;;) {
      final n = tsnxCaptureRead(_e, _blocks, _kCaptureBatch);
      if (n <= 0) break;
      for (var i = 0; i < n; i++) {
        final b = (_blocks + i).ref;
        if (first < 0) first = b.t_ns;
        rate = b.rate;
        final pcm = Int16List(b.frames);
        for (var k = 0; k < b.frames; k++) {
          pcm[k] = b.data[k];
        }
        frames.add(pcm);
      }
    }
    if (frames.isEmpty || !_capture.hasListener) return;
    final total = frames.fold<int>(0, (a, f) => a + f.length);
    final pcm = Int16List(total);
    var off = 0;
    for (final f in frames) {
      pcm.setAll(off, f);
      off += f.length;
    }
    _capture.add(CaptureFrame(pcm: pcm, sampleRate: rate, capturedAtNs: first));
  }

  void _onNotify(int kind, int id, int value) {
    if (_closed) return;
    switch (kind) {
      case _kTrackStarted:
        _tracks[id]?._emit(const TrackStarted());
      case _kTrackStarved:
        _tracks[id]?._emit(const TrackStarved());
      case _kTrackResumed:
        _tracks[id]?._emit(const TrackResumed());
      case _kTrackEnded:
        _tracks[id]?._emit(const TrackEnded());
      case _kTrackFailed:
        _tracks[id]?._emit(const TrackFailed('track storage failed'));
      case _kTrackFlushed:
        _tracks[id]?._flushed(value);
      case _kCaptureReady:
        _drainCapture();
      case _kRequestDone:
        final c = _requests.remove(id);
        if (c == null) return;
        if (value == 0) {
          c.complete();
        } else {
          c.completeError(AudioEngineException(value, _errorText(value)));
        }
      case _kDevicesChanged:
        _routes.add(
          RouteChange(
            inputs: inputs,
            outputs: outputs,
            currentInput: currentInput,
            currentOutput: currentOutput,
          ),
        );
      case _kEngineError:
        developer.log('engine error $value', name: 'telosnex_audio');
    }
  }

  @override
  Future<void> close() async {
    if (_closed) return;
    _closed = true;
    for (final t in _tracks.values.toList()) {
      t._closeLocal();
    }
    _tracks.clear();
    tsnxEngineClose(_e);
    _listener.close();
    for (final c in _requests.values) {
      c.completeError(StateError('AudioEngine closed'));
    }
    _requests.clear();
    calloc
      ..free(_state)
      ..free(_blocks);
    await _capture.close();
    await _routes.close();
  }
}

final class _NativeTrack implements Track {
  _NativeTrack(this._engine, this.id, this.format);

  final _NativeEngine _engine;
  @override
  final int id;
  @override
  final PcmFormat format;
  final StreamController<TrackEvent> _events =
      StreamController<TrackEvent>.broadcast();
  final List<Completer<Duration>> _flushes = [];
  bool _disposed = false;

  Pointer<TsnxEngine> get _e => _engine._e;

  void _checkLive() {
    if (_disposed) throw StateError('Track $id is disposed');
    _engine._checkOpen();
  }

  void _emit(TrackEvent e) {
    if (!_events.isClosed) _events.add(e);
  }

  void _flushed(int frames) {
    final d = format.durationOf(frames);
    for (final c in _flushes) {
      c.complete(d);
    }
    _flushes.clear();
  }

  @override
  int write(Int16List interleaved) {
    _checkLive();
    if (interleaved.length % format.channels != 0) {
      throw ArgumentError('Sample count is not a multiple of the channels');
    }
    final frames = interleaved.length ~/ format.channels;
    if (frames == 0) return 0;
    final r = tsnxTrackWrite(_e, id, interleaved.address, frames);
    switch (r) {
      case _kWriteFull:
        throw TrackFullError('Track $id is full');
      case _kWriteEnded:
        throw StateError('Track $id: write after endOfStream');
      case _kWriteIoError:
        throw const AudioEngineException(-103, 'spill file I/O failed');
    }
    _check(r, 'write');
    return r;
  }

  @override
  void endOfStream() {
    _checkLive();
    _check(tsnxTrackEndOfStream(_e, id), 'endOfStream');
  }

  @override
  void play() {
    _checkLive();
    _check(tsnxTrackPlay(_e, id), 'play');
  }

  @override
  void pause() {
    _checkLive();
    _check(tsnxTrackPause(_e, id), 'pause');
  }

  @override
  void seek(Duration position) {
    _checkLive();
    _check(tsnxTrackSeek(_e, id, format.framesOf(position)), 'seek');
  }

  @override
  void setRate(double rate) {
    _checkLive();
    _check(tsnxTrackSetRate(_e, id, rate), 'setRate');
  }

  @override
  void setGain(
    double gain, {
    Duration ramp = const Duration(milliseconds: 10),
  }) {
    _checkLive();
    _check(tsnxTrackSetGain(_e, id, gain, ramp.inMilliseconds), 'setGain');
  }

  @override
  Future<Duration> flush() {
    _checkLive();
    final c = Completer<Duration>();
    _flushes.add(c);
    final rc = tsnxTrackFlush(_e, id);
    if (rc != 0) {
      _flushes.remove(c);
      return Future.error(AudioEngineException(rc, 'flush: ${_errorText(rc)}'));
    }
    return c.future;
  }

  @override
  TrackState get state {
    if (_disposed) {
      throw StateError('Track $id is disposed');
    }
    return _engine._readState(this);
  }

  @override
  Stream<TrackEvent> get events => _events.stream;

  void _closeLocal() {
    if (_disposed) return;
    _disposed = true;
    for (final c in _flushes) {
      c.completeError(StateError('Track disposed before flush applied'));
    }
    _flushes.clear();
    unawaited(_events.close());
  }

  @override
  Future<void> dispose() async {
    if (_disposed) return;
    if (!_engine._closed) tsnxTrackDispose(_e, id);
    _engine._tracks.remove(id);
    _closeLocal();
  }
}
