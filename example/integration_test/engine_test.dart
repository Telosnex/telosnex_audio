// The engine cases of test/engine_cases.dart inside the example app, with
// the native library that the build hook made for the device (desktop, iOS
// simulator, Android emulator). They use ManualDevice, so they need no audio
// hardware: CI runners and emulators have none that the engine can rely on.
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
}
