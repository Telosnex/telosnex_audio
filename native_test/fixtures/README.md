# Test fixtures

- `shine_24k_mono_64k.mp3`: 16.6 s of macOS `say` speech, 24 kHz mono, encoded
  with `shine_dart` at 64 kbit/s (the app's saved-clip settings, ADR R11).
- `shine_48k_stereo_128k.mp3`: 8 s of the same speech at 48 kHz stereo,
  `shine_dart` at 128 kbit/s.

Both were encoded with the same `ShineConfig` as
`telosnex/lib/squadron/mp3_encoder/mp3_encoder_service.dart`.
