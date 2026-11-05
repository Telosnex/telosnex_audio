// The engine cases of test/engine_cases.dart inside the example app, with
// the native library that the build hook made for the device (desktop, iOS
// simulator, Android emulator). They use ManualDevice, so they need no audio
// hardware: CI runners and emulators have none that the engine can rely on.
// The microphone permission group opens the platform device only where
// capture must fail before it reaches the device.
import 'dart:io';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:telosnex_audio/telosnex_audio.dart';

import '../../test/engine_cases.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  late Directory spill;
  setUp(() {
    spill = Directory.systemTemp.createTempSync('tsnx_integration');
  });
  tearDown(() {
    if (spill.existsSync()) spill.deleteSync(recursive: true);
  });

  engineCases(
    readFixture: (name) async {
      // assets/speech.mp3 is native_test/fixtures/shine_24k_mono_64k.mp3.
      if (name != 'shine_24k_mono_64k.mp3') {
        throw ArgumentError('No asset for fixture $name');
      }
      final data = await rootBundle.load('assets/speech.mp3');
      return data.buffer.asUint8List(data.offsetInBytes, data.lengthInBytes);
    },
    () => TelosnexAudio.open(
      device: const ManualDevice(
        channels: 1,
        delay: Duration(milliseconds: 40),
      ),
      spillDir: spill.path,
    ),
  );

  group('microphone permission', () {
    test('status, and a request that needs no prompt', () async {
      final status = await TelosnexAudio.microphonePermission();
      if (Platform.isLinux || Platform.isWindows) {
        expect(status, MicrophonePermission.granted);
      }
      if (status == MicrophonePermission.notDetermined) {
        markTestSkipped('$status: a request would show the prompt');
        return;
      }
      expect(await TelosnexAudio.requestMicrophonePermission(), status);
    });

    // Since WebRTC M144 the Apple voice-processing device no longer asks;
    // without the permission, capture fails at once with a clear code.
    test('capture without it fails with noMicrophonePermission', () async {
      final status = await TelosnexAudio.microphonePermission();
      final checked = Platform.isIOS || Platform.isMacOS || Platform.isAndroid;
      if (!checked || status == MicrophonePermission.granted) {
        markTestSkipped('$status on ${Platform.operatingSystem}');
        return;
      }
      final engine = await TelosnexAudio.open(
        device: const PlatformDevice(appleVoiceProcessing: true),
        spillDir: spill.path,
      );
      try {
        await expectLater(
          engine.startCapture(const CaptureConfig(sampleRate: 48000)),
          throwsA(
            isA<AudioEngineException>().having(
              (e) => e.code,
              'code',
              AudioEngineException.noMicrophonePermission,
            ),
          ),
        );
      } finally {
        await engine.close();
      }
    });
  });
}
