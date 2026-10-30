# telosnex_audio

An audio engine for Flutter: microphone capture with WebRTC echo
cancellation, noise suppression and automatic gain, and a mixer of playback
tracks you can seek, pause, and play faster or slower. One API on Android,
iOS, macOS, Linux, Windows and the web.

```dart
import 'package:telosnex_audio/telosnex_audio.dart';

Future<void> main() async {
  if (!TelosnexAudio.isSupported) return;
  final engine = await TelosnexAudio.open();

  // Playback: write 16-bit PCM, then control the track.
  final track = engine.createTrack(
    const PcmFormat(sampleRate: 24000, channels: 1),
  );
  track.write(pcm); // Int16List, interleaved
  track.endOfStream();
  track.setRate(1.25);
  track.play();

  // Capture: echo-cancelled against what the engine plays.
  engine.capture.listen((frame) => send(frame.pcm));
  await engine.startCapture(const CaptureConfig());
}
```

- **Echo cancellation** uses what the engine itself plays, so a voice
  assistant can listen while it speaks.
- **Tracks:** `play`, `pause`, `seek`, `setRate`, `setGain` (with a ramp),
  and `createMp3Track` for MP3 bytes.
- **Devices:** `inputs`, `outputs`, `selectInput`, `selectOutput`, and
  `routeChanges` when headphones come and go.

The native libraries are prebuilt for every target
([Kiln](https://pub.dev/packages/kiln)): the build hook downloads the one
for your target and checks its SHA-256. It builds from source when the
sources differ from the release. On the web, the engine runs in an
AudioWorklet with WebAssembly.

## License

MIT. The bundled code keeps its own licenses (WebRTC, Abseil, minimp3,
Sonic), all in LICENSE.
