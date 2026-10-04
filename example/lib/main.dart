// Development app for telosnex_audio: play the speech fixture with pause,
// seek, and rate, and show the echo-cancelled microphone level.
import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:telosnex_audio/telosnex_audio.dart';

void main() => runApp(const MaterialApp(home: ExamplePage()));

class ExamplePage extends StatefulWidget {
  const ExamplePage({super.key});
  @override
  State<ExamplePage> createState() => _ExamplePageState();
}

class _ExamplePageState extends State<ExamplePage> {
  AudioEngine? _engine;
  Track? _track;
  Timer? _tick;
  String _status = 'opening';
  double _micDb = -90;
  bool _capturing = false;
  double _rate = 1;
  StreamSubscription<CaptureFrame>? _capture;

  @override
  void initState() {
    super.initState();
    unawaited(_open());
  }

  Future<void> _open() async {
    if (!TelosnexAudio.isSupported) {
      setState(() => _status = 'no native engine on this platform');
      return;
    }
    final engine = await TelosnexAudio.open();
    final mp3 = await rootBundle.load('assets/speech.mp3');
    final track = await engine.createMp3Track(mp3.buffer.asUint8List());
    _capture = engine.capture.listen((f) {
      var e = 0.0;
      for (final s in f.pcm) {
        e += s * s;
      }
      _micDb = 10 * math.log(e / f.pcm.length + 1e-9) / math.ln10;
    });
    setState(() {
      _engine = engine;
      _track = track;
      _status = 'ready';
    });
    _tick = Timer.periodic(
      const Duration(milliseconds: 50),
      (_) => setState(() {}),
    );
  }

  @override
  void dispose() {
    _tick?.cancel();
    unawaited(_capture?.cancel());
    unawaited(_engine?.close());
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final engine = _engine;
    final track = _track;
    if (engine == null || track == null) {
      return Scaffold(body: Center(child: Text(_status)));
    }
    final s = track.state;
    final pos = s.positionAt(engine.nowNs());
    final dur = s.duration ?? Duration.zero;
    return Scaffold(
      appBar: AppBar(title: const Text('telosnex_audio')),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              '${s.status.name}  ${_fmt(pos)} / ${_fmt(dur)}  '
              'rate ${s.rate}  delay ${engine.outputDelay.inMilliseconds} ms',
            ),
            Slider(
              value: pos.inMilliseconds.clamp(0, dur.inMilliseconds).toDouble(),
              max: math.max(1, dur.inMilliseconds).toDouble(),
              onChanged: (v) => track.seek(Duration(milliseconds: v.round())),
            ),
            Wrap(
              spacing: 8,
              children: [
                FilledButton(
                  onPressed: () {
                    if (s.status == TrackStatus.ended) {
                      track.seek(Duration.zero);
                    }
                    s.isPlaying ? track.pause() : track.play();
                  },
                  child: Text(s.isPlaying ? 'Pause' : 'Play'),
                ),
                for (final r in [1.0, 1.5, 2.0])
                  ChoiceChip(
                    label: Text('${r}x'),
                    selected: _rate == r,
                    onSelected: (_) {
                      track.setRate(r);
                      setState(() => _rate = r);
                    },
                  ),
              ],
            ),
            const Divider(height: 32),
            Row(
              children: [
                FilledButton.tonal(
                  onPressed: () async {
                    if (_capturing) {
                      await engine.stopCapture();
                    } else {
                      await engine.startCapture(
                        const CaptureConfig(sampleRate: 24000),
                      );
                    }
                    setState(() => _capturing = !_capturing);
                  },
                  child: Text(_capturing ? 'Stop mic' : 'Start mic'),
                ),
                const SizedBox(width: 16),
                Text(
                  'mic after echo cancellation: ${_micDb.toStringAsFixed(1)} dB',
                ),
              ],
            ),
            const SizedBox(height: 16),
            Text('Outputs: ${engine.outputs.map((d) => d.name).join(', ')}'),
            Text('Inputs: ${engine.inputs.map((d) => d.name).join(', ')}'),
          ],
        ),
      ),
    );
  }

  static String _fmt(Duration d) =>
      '${d.inMinutes}:${(d.inSeconds % 60).toString().padLeft(2, '0')}.'
      '${(d.inMilliseconds % 1000 ~/ 100)}';
}
