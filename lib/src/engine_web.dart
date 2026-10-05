// The web engine (ADR D14, D15). The Track core runs as WebAssembly in an
// AudioWorklet (lib/web/telosnex_audio_worklet.js); a worker keeps the
// backing store (lib/web/telosnex_audio_storage.js). Dart sends commands
// and PCM over MessagePorts and keeps the last state the worklet posted, so
// no call waits (D10).
//
// Clock: the engine clock is the AudioContext render clock (the frame being
// rendered, in ns). The worklet's device delay is baseLatency +
// outputLatency; nowNs() maps the heard time from getOutputTimestamp() back
// to the render clock with the same delay.
//
// ManualDevice runs the same core on this thread, for tests.
import 'dart:async';
import 'dart:developer' as developer;
import 'dart:js_interop';
import 'dart:js_interop_unsafe';
import 'dart:math' as math;
import 'dart:typed_data';
import 'dart:ui_web' as ui_web;

import 'package:web/web.dart' as web;

import 'types.dart';

// Flutter assets in an app. `flutter test --platform chrome` has no asset
// bundle but serves package:telosnex_audio/ at /packages/telosnex_audio/.
String _assetUrl(String file) => _assetBase == null
    ? ui_web.assetManager.getAssetUrl('packages/telosnex_audio/lib/web/$file')
    : '$_assetBase/$file';
String? _assetBase;
const _testAssetBase = 'packages/telosnex_audio/web';

bool get engineSupported =>
    globalContext.has('AudioWorkletNode') && globalContext.has('WebAssembly');

Future<AudioEngine> openEngine({
  required EngineDevice device,
  required AudioProcessingConfig processing,
  String? spillDir,
}) {
  if (!engineSupported) {
    throw UnsupportedError('This browser has no AudioWorklet or WebAssembly');
  }
  return _WebEngine.open(device, processing);
}

@JS('WebAssembly.compile')
external JSPromise<JSObject> _wasmCompile(JSArrayBuffer bytes);

const _kTrackStarted = 1;
const _kTrackStarved = 2;
const _kTrackResumed = 3;
const _kTrackEnded = 4;
const _kTrackFailed = 5;
const _kTrackFlushed = 6;

const _kPlay = 0;
const _kPause = 1;
const _kSeek = 2;
const _kRate = 3;
const _kGain = 4;
const _kFlush = 5;

const _kMaxTracks = 8;
const _kAheadSeconds = 30;
const _kMaxSeconds = 2 * 60 * 60;

String _errorText(int code) => switch (code) {
  -1 => 'invalid argument',
  -2 => 'no free track (8 maximum)',
  -3 => 'no such track',
  -4 => 'device error',
  -5 => 'unsupported format',
  -6 => 'command queue full',
  _ => 'error $code',
};

JSObject _msg(String type, [Map<String, Object?> fields = const {}]) {
  final m = JSObject()..['t'] = type.toJS;
  for (final MapEntry(:key, :value) in fields.entries) {
    m[key] = switch (value) {
      null => null,
      final int v => v.toJS,
      final double v => v.toJS,
      final bool v => v.toJS,
      final String v => v.toJS,
      _ => value as JSAny,
    };
  }
  return m;
}

JSObject _bufferOf(JSTypedArray a) => a.getProperty<JSObject>('buffer'.toJS);

extension type _Message(JSObject _) implements JSObject {
  external String get t;
  external num get id;
}

final class _WebEngine implements AudioEngine {
  _WebEngine._(this._processing, this._manual);

  static Future<_WebEngine> open(
    EngineDevice device,
    AudioProcessingConfig processing,
  ) async {
    final e = _WebEngine._(processing, device is ManualDevice ? device : null);
    try {
      await e._start();
    } catch (_) {
      await e.close();
      rethrow;
    }
    return e;
  }

  final AudioProcessingConfig _processing;
  final ManualDevice? _manual;

  web.AudioContext? _ctx;
  web.AudioWorkletNode? _node;
  JSObject? _core; // ManualDevice: TsnxCore on this thread
  web.Worker? _worker;
  final String _session =
      '${DateTime.now().microsecondsSinceEpoch}-'
      '${math.Random().nextInt(1 << 30)}';

  final Map<int, _WebTrack> _tracks = {};
  final Map<int, Completer<JSObject>> _mp3Opens = {};
  int _nextId = 1;
  bool _closed = false;
  int _lastTickNs = 0;
  int _manualNowNs = 0;
  double _latencySeconds = 0;
  Timer? _latencyTimer;
  bool _latencyFixed = false;

  // Output demand (the native idle stop): the context is suspended after
  // 3 s with nothing to play and no capture, and resumed on demand.
  static const _idleStop = Duration(seconds: 3);
  Timer? _idleTimer;
  DateTime _lastDemand = DateTime.now();
  bool _suspending = false;
  bool _capturing = false;
  bool _idlePosted = false;
  String _ctxState = 'suspended';
  final List<(web.EventTarget, String, JSFunction)> _listeners = [];

  // Capture and devices.
  final StreamController<CaptureFrame> _capture =
      StreamController<CaptureFrame>.broadcast(sync: true);
  final StreamController<RouteChange> _routes =
      StreamController<RouteChange>.broadcast();
  web.MediaStream? _mic;
  web.MediaStreamAudioSourceNode? _micSource;
  String? _inputId;
  List<AudioDevice> _inputs = const [];
  List<AudioDevice> _outputs = const [
    AudioDevice(id: 'default', name: 'System default'),
  ];

  Future<void> _start() async {
    var wasmResponse = await web.window
        .fetch(_assetUrl('telosnex_audio.wasm').toJS)
        .toDart;
    if (wasmResponse.status == 404 && _assetBase == null) {
      _assetBase = _testAssetBase;
      wasmResponse = await web.window
          .fetch(_assetUrl('telosnex_audio.wasm').toJS)
          .toDart;
    }
    if (!wasmResponse.ok) {
      throw AudioEngineException(
        -4,
        'telosnex_audio.wasm: HTTP ${wasmResponse.status}',
      );
    }
    final wasm = await wasmResponse.arrayBuffer().toDart;

    final worker = web.Worker(_assetUrl('telosnex_audio_storage.js').toJS);
    _worker = worker;
    worker.onmessage = ((web.MessageEvent e) {
      _onStorage(e.data! as JSObject);
    }).toJS;
    worker.postMessage(_msg('init', {'wasm': wasm, 'session': _session}));
    final channel = web.MessageChannel();
    worker.postMessage(
      _msg('port', {'port': channel.port2}),
      <JSObject>[channel.port2].toJS,
    );

    final manual = _manual;
    if (manual != null) {
      await _loadCoreScript();
      final module = await _wasmCompile(wasm).toDart;
      final post = ((JSObject m, JSAny? _) => _onCore(m)).toJS;
      _core = (globalContext['TsnxCore']! as JSFunction).callAsConstructor(
        module,
        post,
        48000.toJS,
      );
      _send(_msg('storage', {'port': channel.port1}));
      _send(_msg('delay', {'ns': manual.delay.inMicroseconds * 1000}));
      _outputs = const [];
      return;
    }

    final ctx = web.AudioContext(
      web.AudioContextOptions(
        latencyHint: 'interactive'.toJS,
        sampleRate: 48000,
      ),
    );
    _ctx = ctx;
    await ctx.audioWorklet
        .addModule(_assetUrl('telosnex_audio_worklet.js'))
        .toDart;
    final options = JSObject()..['wasm'] = wasm;
    final node = web.AudioWorkletNode(
      ctx,
      'telosnex-audio',
      web.AudioWorkletNodeOptions(
        numberOfInputs: 1,
        numberOfOutputs: 1,
        outputChannelCount: <JSNumber>[2.toJS].toJS,
        processorOptions: options,
      ),
    );
    _node = node;
    node.port.onmessage = ((web.MessageEvent e) {
      _onCore(e.data! as JSObject);
    }).toJS;
    node.onprocessorerror = ((web.Event _) {
      developer.log('worklet processor error', name: 'telosnex_audio');
      for (final t in _tracks.values) {
        t._emit(const TrackFailed('audio worklet failed'));
      }
    }).toJS;
    node.connect(ctx.destination);
    _send(_msg('storage', {'port': channel.port1}), <JSObject>[channel.port1]);
    _updateLatency();
    _latencyTimer = Timer.periodic(
      const Duration(seconds: 1),
      (_) => _updateLatency(),
    );
    _ctxState = ctx.state;
    _listen(ctx, 'statechange', (web.Event _) => _onStateChange());
    _idleTimer = Timer.periodic(
      const Duration(milliseconds: 500),
      (_) => _checkIdle(),
    );
    // Autoplay rules: the context runs after the first user gesture.
    unawaited(_resume());
    for (final type in ['pointerdown', 'keydown', 'touchend']) {
      _listen(web.document, type, (web.Event _) => unawaited(_resume()));
    }
    final media = web.window.navigator.mediaDevices;
    _listen(media, 'devicechange', (web.Event _) async {
      await _refreshDevices();
      if (!_routes.isClosed) {
        _routes.add(RouteChange(inputs: _inputs, outputs: _outputs));
      }
    });
    await _refreshDevices();
  }

  void _listen(
    web.EventTarget target,
    String type,
    void Function(web.Event) fn,
  ) {
    final f = fn.toJS;
    target.addEventListener(type, f);
    _listeners.add((target, type, f));
  }

  Future<void> _resume() async {
    _lastDemand = DateTime.now();
    final ctx = _ctx;
    if (ctx == null || _closed || ctx.state == 'running') return;
    try {
      await ctx.resume().toDart;
    } catch (_) {
      // Not allowed yet; a later gesture retries.
    }
  }

  void _onStateChange() {
    final ctx = _ctx;
    if (ctx == null) return;
    final was = _ctxState;
    _ctxState = ctx.state;
    if (was == 'running' && _ctxState != 'running') {
      // The engine suspends only when nothing plays. Otherwise the browser
      // stopped the output (an interruption or a device change) and the
      // audio in its buffer was not heard (ADR I10).
      if (!_suspending) _send(_msg('restart'));
      _suspending = false;
      _postIdle();
    }
  }

  bool get _hasDemand =>
      _capturing ||
      _tracks.values.any((t) {
        final w = t._words;
        final status = w == null ? 0 : w[1].toInt();
        return t._wantsPlay ||
            status == TrackStatus.playing.index ||
            status == TrackStatus.starved.index;
      });

  void _checkIdle() {
    final ctx = _ctx;
    if (ctx == null || _closed) return;
    if (_hasDemand) {
      _lastDemand = DateTime.now();
      return;
    }
    if (ctx.state != 'running' ||
        DateTime.now().difference(_lastDemand) < _idleStop) {
      return;
    }
    _suspending = true;
    unawaited(
      ctx.suspend().toDart.then(
        (_) {},
        onError: (Object _) {
          _suspending = false;
        },
      ),
    );
  }

  /// Without rendering, the worklet applies commands only when asked.
  void _postIdle() {
    final ctx = _ctx;
    if (ctx == null || ctx.state == 'running' || _idlePosted) return;
    _idlePosted = true;
    scheduleMicrotask(() {
      _idlePosted = false;
      if (!_closed) _node?.port.postMessage(_msg('idle'));
    });
  }

  static Future<void> _loadCoreScript() async {
    if (globalContext.has('TsnxCore')) return;
    final script = web.document.createElement('script') as web.HTMLScriptElement
      ..src = _assetUrl('telosnex_audio_worklet.js');
    final done = Completer<void>();
    script
      ..onload = ((web.Event _) => done.complete()).toJS
      ..onerror = ((web.Event _) => done.completeError(
        const AudioEngineException(-4, 'telosnex_audio_worklet.js failed'),
      )).toJS;
    web.document.head!.append(script);
    await done.future;
  }

  void _updateLatency() {
    final ctx = _ctx;
    if (ctx == null || _latencyFixed) return;
    double seconds(String name) {
      final v = ctx.getProperty<JSAny?>(name.toJS);
      return v.isA<JSNumber>() ? (v! as JSNumber).toDartDouble : 0;
    }

    final latency = seconds('baseLatency') + seconds('outputLatency');
    if ((latency - _latencySeconds).abs() < 0.001 && _latencyTimer != null) {
      return;
    }
    _latencySeconds = latency;
    _send(_msg('delay', {'ns': (latency * 1e9).round()}));
  }

  void _send(JSObject msg, [List<JSObject>? transfer]) {
    final core = _core;
    if (core != null) {
      core.callMethod('handle'.toJS, msg);
      return;
    }
    final node = _node;
    if (node == null) return;
    if (transfer == null) {
      node.port.postMessage(msg);
    } else {
      node.port.postMessage(msg, transfer.toJS);
    }
    _postIdle();
  }

  void _toStorage(JSObject msg, [List<JSObject>? transfer]) {
    final w = _worker;
    if (w == null) return;
    if (transfer == null) {
      w.postMessage(msg);
    } else {
      w.postMessage(msg, transfer.toJS);
    }
  }

  void _checkOpen() {
    if (_closed) throw StateError('AudioEngine is closed');
  }

  // ---- Messages ----

  void _onCore(JSObject raw) {
    if (_closed) return;
    final m = raw as _Message;
    switch (m.t) {
      case 'tick':
        _lastTickNs = (m['now']! as JSNumber).toDartDouble.round();
        final s = (m['s']! as JSFloat64Array).toDart;
        for (var i = 0; i + 9 <= s.length; i += 9) {
          final t = _tracks[s[i].toInt()];
          if (t == null) continue;
          t._words = Float64List.sublistView(s, i, i + 9);
          // The status now covers the demand from play().
          if (s[i + 1] != TrackStatus.idle.index &&
              s[i + 1] != TrackStatus.paused.index) {
            t._wantsPlay = false;
          }
        }
        final e = (m['e']! as JSFloat64Array).toDart;
        for (var i = 0; i + 3 <= e.length; i += 3) {
          _onEvent(e[i].toInt(), e[i + 1].toInt(), e[i + 2].toInt());
        }
      case 'cap':
        if (!_capture.hasListener) return;
        _capture.add(
          CaptureFrame(
            pcm: (m['pcm']! as JSInt16Array).toDart,
            sampleRate: (m['rate']! as JSNumber).toDartInt,
            capturedAtNs: (m['at']! as JSNumber).toDartDouble.round(),
          ),
        );
      case 'err':
        final code = (m['code']! as JSNumber).toDartInt;
        final op = (m['op']! as JSString).toDart;
        developer.log(
          'core $op on track ${m.id}: ${_errorText(code)}',
          name: 'telosnex_audio',
        );
        _tracks[m.id.toInt()]?._emit(TrackFailed('$op: ${_errorText(code)}'));
    }
  }

  void _onEvent(int kind, int id, int value) {
    final t = _tracks[id];
    if (t == null) return;
    switch (kind) {
      case _kTrackStarted:
        t._emit(const TrackStarted());
      case _kTrackStarved:
        t._emit(const TrackStarved());
      case _kTrackResumed:
        t._emit(const TrackResumed());
      case _kTrackEnded:
        t._emit(const TrackEnded());
      case _kTrackFailed:
        t._emit(const TrackFailed('track storage failed'));
      case _kTrackFlushed:
        t._flushed(value);
    }
  }

  void _onStorage(JSObject raw) {
    final m = raw as _Message;
    if (m.t == 'mp3') _mp3Opens.remove(m.id.toInt())?.complete(m);
  }

  // ---- AudioEngine ----

  @override
  int nowNs() {
    if (_manual != null) return _manualNowNs;
    final ctx = _ctx!;
    if (ctx.state != 'running') return _lastTickNs;
    final ts = ctx.getOutputTimestamp();
    final perf = ts.performanceTime;
    if (perf <= 0) return _lastTickNs;
    final now = web.window.performance.now();
    final render = ts.contextTime + (now - perf) / 1000 + _latencySeconds;
    return (render * 1e9).round();
  }

  @override
  Duration get outputDelay =>
      _manual?.delay ?? Duration(microseconds: (_latencySeconds * 1e6).round());

  int _reserveId() {
    _checkOpen();
    if (_tracks.length + _mp3Opens.length >= _kMaxTracks) {
      throw AudioEngineException(-2, 'createTrack: ${_errorText(-2)}');
    }
    return _nextId++;
  }

  @override
  Track createTrack(PcmFormat format, {Retention retention = Retention.all}) {
    _checkOpen();
    final rate = format.sampleRate;
    if (rate < 8000 || rate > 48000 || rate % 100 != 0) {
      throw AudioEngineException(-5, 'createTrack: ${_errorText(-5)}');
    }
    final id = _reserveId();
    final all = retention == Retention.all;
    _send(
      _msg('create', {
        'id': id,
        'rate': rate,
        'ch': format.channels,
        'retention': all ? 0 : 1,
      }),
    );
    if (all) {
      _toStorage(_msg('pcm', {'id': id, 'rate': rate, 'ch': format.channels}));
    }
    return _tracks[id] = _WebTrack(this, id, format, retention, null);
  }

  @override
  Future<Track> createMp3Track(Uint8List mp3) async {
    final id = _reserveId();
    final done = Completer<JSObject>();
    _mp3Opens[id] = done;
    final bytes = Uint8List.fromList(mp3).buffer.toJS;
    _toStorage(_msg('mp3', {'id': id, 'bytes': bytes}), [bytes]);
    final reply = await done.future;
    _checkOpen();
    if (reply.has('error')) {
      throw AudioEngineException(-5, 'createMp3Track: ${_errorText(-5)}');
    }
    final rate = (reply['rate']! as JSNumber).toDartInt;
    final channels = (reply['ch']! as JSNumber).toDartInt;
    final frames = (reply['frames']! as JSNumber).toDartDouble.round();
    _send(
      _msg('whole', {'id': id, 'rate': rate, 'ch': channels, 'frames': frames}),
    );
    return _tracks[id] = _WebTrack(
      this,
      id,
      PcmFormat(sampleRate: rate, channels: channels),
      Retention.all,
      frames,
    );
  }

  @override
  Future<void> startCapture(CaptureConfig config) async {
    _checkOpen();
    final rate = config.sampleRate;
    if (rate < 8000 || rate > 48000 || rate % 100 != 0) {
      throw AudioEngineException(-1, 'startCapture: ${_errorText(-1)}');
    }
    _capturing = true;
    final ctx = _ctx;
    if (ctx != null && _mic == null) await _openMic(ctx);
    _send(_msg('capture', {'rate': rate}));
  }

  Future<void> _openMic(web.AudioContext ctx) async {
    final audio = JSObject()
      ..['echoCancellation'] = _processing.echoCancellation.toJS
      ..['noiseSuppression'] = _processing.noiseSuppression.toJS
      ..['autoGainControl'] = _processing.autoGain.toJS;
    final input = _inputId;
    if (input != null && input != 'default') {
      audio['deviceId'] = (JSObject()..['exact'] = input.toJS);
    }
    final stream = await web.window.navigator.mediaDevices
        .getUserMedia(web.MediaStreamConstraints(audio: audio))
        .toDart;
    if (_closed) {
      _stopStream(stream);
      return;
    }
    _mic = stream;
    final source = ctx.createMediaStreamSource(stream);
    _micSource = source;
    source.connect(_node!);
    unawaited(_resume());
    // Device labels are visible after the permission.
    await _refreshDevices();
  }

  void _closeMic() {
    _micSource?.disconnect();
    _micSource = null;
    final mic = _mic;
    _mic = null;
    if (mic != null) _stopStream(mic);
  }

  static void _stopStream(web.MediaStream stream) {
    for (final t in stream.getTracks().toDart) {
      t.stop();
    }
  }

  @override
  Future<void> stopCapture() async {
    _checkOpen();
    _capturing = false;
    _send(_msg('capture', {'rate': 0}));
    _closeMic();
  }

  @override
  Stream<CaptureFrame> get capture => _capture.stream;

  Future<void> _refreshDevices() async {
    if (_manual != null) return;
    final List<web.MediaDeviceInfo> all;
    try {
      all = (await web.window.navigator.mediaDevices.enumerateDevices().toDart)
          .toDart;
    } catch (_) {
      return;
    }
    var mics = 0;
    var speakers = 0;
    final inputs = <AudioDevice>[];
    final outputs = <AudioDevice>[];
    for (final d in all) {
      if (d.kind == 'audioinput') {
        mics++;
        inputs.add(
          AudioDevice(
            id: d.deviceId.isEmpty ? 'default' : d.deviceId,
            name: d.label.isEmpty ? 'Microphone $mics' : d.label,
          ),
        );
      } else if (d.kind == 'audiooutput' && _canSelectOutput) {
        speakers++;
        outputs.add(
          AudioDevice(
            id: d.deviceId.isEmpty ? 'default' : d.deviceId,
            name: d.label.isEmpty ? 'Speaker $speakers' : d.label,
          ),
        );
      }
    }
    _inputs = List.unmodifiable(inputs);
    if (_canSelectOutput && outputs.isNotEmpty) {
      _outputs = List.unmodifiable(outputs);
    }
  }

  bool get _canSelectOutput => _ctx?.has('setSinkId') ?? false;

  @override
  List<AudioDevice> get inputs {
    _checkOpen();
    return _inputs;
  }

  @override
  List<AudioDevice> get outputs {
    _checkOpen();
    return _outputs;
  }

  @override
  Future<void> selectInput(String deviceId) async {
    _checkOpen();
    _inputId = deviceId;
    final ctx = _ctx;
    if (ctx != null && _mic != null) {
      _closeMic();
      await _openMic(ctx);
    }
  }

  @override
  Future<void> selectOutput(String deviceId) async {
    _checkOpen();
    final ctx = _ctx;
    if (ctx == null) return;
    if (!_canSelectOutput) {
      if (deviceId == 'default') return;
      throw const AudioEngineException(
        -4,
        'selectOutput: this browser cannot choose the output device',
      );
    }
    final id = deviceId == 'default' ? '' : deviceId;
    await ctx.callMethod<JSPromise<JSAny?>>('setSinkId'.toJS, id.toJS).toDart;
  }

  @override
  Stream<RouteChange> get routeChanges => _routes.stream;

  @override
  Int16List renderManual(int blocks, {Int16List? captureIn}) {
    _checkOpen();
    final manual = _manual;
    final core = _core;
    if (manual == null || core == null) {
      throw StateError('renderManual needs a ManualDevice');
    }
    if (captureIn != null && captureIn.length != blocks * 480) {
      throw ArgumentError.value(
        captureIn.length,
        'captureIn',
        'must hold $blocks blocks of 480 mono samples',
      );
    }
    final start = _manualNowNs;
    _manualNowNs += blocks * 10000000;
    final out = core.callMethod<JSInt16Array>(
      'renderManual'.toJS,
      blocks.toJS,
      manual.channels.toJS,
      start.toJS,
      captureIn?.toJS,
    );
    return out.toDart;
  }

  @override
  Future<void> close() async {
    if (_closed) return;
    _closed = true;
    _latencyTimer?.cancel();
    _idleTimer?.cancel();
    for (final (target, type, f) in _listeners) {
      target.removeEventListener(type, f);
    }
    _listeners.clear();
    for (final t in _tracks.values.toList()) {
      t._closeLocal();
    }
    _tracks.clear();
    for (final c in _mp3Opens.values) {
      c.completeError(StateError('AudioEngine closed'));
    }
    _mp3Opens.clear();
    _closeMic();
    _toStorage(_msg('close'));
    _worker = null;
    _node?.disconnect();
    _node = null;
    _core = null;
    final ctx = _ctx;
    _ctx = null;
    if (ctx != null) {
      try {
        await ctx.close().toDart;
      } catch (_) {}
    }
    await _capture.close();
    await _routes.close();
  }
}

final class _WebTrack implements Track {
  _WebTrack(this._engine, this.id, this.format, this._retention, int? whole)
    : _written = whole ?? 0,
      _ended = whole != null;

  final _WebEngine _engine;
  @override
  final int id;
  @override
  final PcmFormat format;
  final Retention _retention;
  final StreamController<TrackEvent> _events =
      StreamController<TrackEvent>.broadcast();
  final List<Completer<Duration>> _flushes = [];
  int _written;
  bool _ended;
  bool _disposed = false;

  /// play() was called and no state since shows the track stopped.
  bool _wantsPlay = false;

  /// The last state the core posted: id, status, position, sampled_at,
  /// rate, written, duration, mixer position, underruns.
  Float64List? _words;

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

  void _command(int kind, {int i = 0, double d = 0}) {
    _checkLive();
    _engine._send(_msg('cmd', {'id': id, 'k': kind, 'i': i, 'd': d}));
  }

  @override
  int write(Int16List interleaved) {
    _checkLive();
    if (interleaved.length % format.channels != 0) {
      throw ArgumentError('Sample count is not a multiple of the channels');
    }
    if (_ended) throw StateError('Track $id: write after endOfStream');
    final frames = interleaved.length ~/ format.channels;
    if (frames == 0) return 0;
    final rate = format.sampleRate;
    if (_retention == Retention.unplayed) {
      final playhead = _words == null ? 0 : _words![7].toInt();
      if (_written + frames - playhead > _kAheadSeconds * rate) {
        throw TrackFullError('Track $id is full');
      }
    } else if (_written + frames > _kMaxSeconds * rate) {
      throw TrackFullError('Track $id is full');
    }
    _written += frames;
    final ch = format.channels;
    final pcm = Int16List.fromList(interleaved).toJS;
    _engine._send(_msg('write', {'id': id, 'ch': ch, 'pcm': pcm}), [
      _bufferOf(pcm),
    ]);
    if (_retention == Retention.all) {
      final copy = Int16List.fromList(interleaved).toJS;
      _engine._toStorage(_msg('write', {'id': id, 'pcm': copy}), [
        _bufferOf(copy),
      ]);
    }
    return frames;
  }

  @override
  void endOfStream() {
    _checkLive();
    if (_ended) return;
    _ended = true;
    _engine._send(_msg('eos', {'id': id}));
    if (_retention == Retention.all) {
      _engine._toStorage(_msg('eos', {'id': id}));
    }
  }

  @override
  void play() {
    _command(_kPlay);
    _wantsPlay = true;
    unawaited(_engine._resume());
  }

  @override
  void pause() {
    _command(_kPause);
    _wantsPlay = false;
  }

  @override
  void seek(Duration position) =>
      _command(_kSeek, i: format.framesOf(position));

  @override
  void setRate(double rate) {
    if (!(rate >= 0.5 && rate <= 3.0)) {
      throw AudioEngineException(-1, 'setRate: ${_errorText(-1)}');
    }
    _command(_kRate, d: rate);
  }

  @override
  void setGain(
    double gain, {
    Duration ramp = const Duration(milliseconds: 10),
  }) {
    if (!(gain >= 0.0 && gain <= 4.0) || ramp.isNegative) {
      throw AudioEngineException(-1, 'setGain: ${_errorText(-1)}');
    }
    _command(_kGain, i: ramp.inMilliseconds, d: gain);
  }

  @override
  Future<Duration> flush() {
    _checkLive();
    final c = Completer<Duration>();
    _flushes.add(c);
    _command(_kFlush);
    return c.future;
  }

  @override
  TrackState get state {
    if (_disposed) throw StateError('Track $id is disposed');
    final w = _words;
    if (w == null) {
      return TrackState(
        status: TrackStatus.idle,
        position: Duration.zero,
        sampledAtNs: _engine.nowNs(),
        rate: 1,
        buffered: format.durationOf(_written),
        duration: _ended ? format.durationOf(_written) : null,
        writtenFrames: _written,
        positionFrames: 0,
      );
    }
    final position = w[2].toInt();
    final written = w[5].toInt();
    final duration = w[6].toInt();
    return TrackState(
      status: TrackStatus.values[w[1].toInt()],
      position: format.durationOf(position),
      sampledAtNs: w[3].toInt(),
      rate: w[4],
      buffered: format.durationOf(math.max(0, written - position)),
      duration: duration < 0 ? null : format.durationOf(duration),
      writtenFrames: written,
      positionFrames: position,
    );
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
    if (!_engine._closed) {
      _engine._send(_msg('dispose', {'id': id}));
      if (_retention == Retention.all) {
        _engine._toStorage(_msg('dispose', {'id': id}));
      }
    }
    _engine._tracks.remove(id);
    _closeLocal();
  }
}

// ---- Test probes (lib/web_testing.dart) ----

/// Output samples recorded after the engine, with the context frame of
/// each piece. The frame clock is the engine clock: frame f is at
/// f / sampleRate seconds.
final class WebOutputTap {
  WebOutputTap._(this.sampleRate, this._node);

  final int sampleRate;
  final web.AudioWorkletNode _node;
  final List<(int, Float32List)> pieces = [];

  void start() {
    pieces.clear();
    _node.port.postMessage(JSObject()..['on'] = true.toJS);
  }

  void stop() => _node.port.postMessage(JSObject()..['on'] = false.toJS);
}

Future<WebOutputTap> debugTapOutput(AudioEngine engine) async {
  final e = engine as _WebEngine;
  final ctx = e._ctx!;
  final node = e._node!;
  final tap = web.AudioWorkletNode(
    ctx,
    'telosnex-tap',
    web.AudioWorkletNodeOptions(
      numberOfInputs: 1,
      numberOfOutputs: 1,
      outputChannelCount: <JSNumber>[2.toJS].toJS,
    ),
  );
  final t = WebOutputTap._(ctx.sampleRate.round(), tap);
  tap.port.onmessage = ((web.MessageEvent m) {
    final d = m.data! as JSObject;
    t.pieces.add((
      (d['frame']! as JSNumber).toDartInt,
      (d['pcm']! as JSFloat32Array).toDart,
    ));
  }).toJS;
  node
    ..disconnect()
    ..connect(tap);
  tap.connect(ctx.destination);
  return t;
}

/// Fixes the device delay the worklet uses (normally baseLatency +
/// outputLatency). With zero, the heard position is the position at the
/// render clock, which a tap can check.
void debugSetOutputDelay(AudioEngine engine, Duration delay) {
  final e = engine as _WebEngine
    .._latencyFixed = true
    .._latencySeconds = delay.inMicroseconds / 1e6;
  e._send(_msg('delay', {'ns': delay.inMicroseconds * 1000}));
}

/// The AudioContext state ('running', 'suspended', 'closed').
String debugContextState(AudioEngine engine) =>
    (engine as _WebEngine)._ctx?.state ?? 'manual';

/// Resumes the AudioContext (call from a user gesture where needed).
Future<void> debugResume(AudioEngine engine) =>
    (engine as _WebEngine)._resume();

void debugIdle(AudioEngine engine) =>
    (engine as _WebEngine)._send(_msg('idle'));
