// Dart API tests with the manual device (ADR workplan step 5 gate), on the
// VM through FFI. The cases are in engine_cases.dart.
@TestOn('vm')
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:telosnex_audio/telosnex_audio.dart';

import 'engine_cases.dart';

void main() {
  late Directory spill;

  setUp(() {
    spill = Directory.systemTemp.createTempSync('tsnx_dart_test');
  });

  tearDown(() {
    if (spill.existsSync()) spill.deleteSync(recursive: true);
  });

  engineCases(
    readFixture: (name) async =>
        File('native_test/fixtures/$name').readAsBytesSync(),
    () => TelosnexAudio.open(
      device: const ManualDevice(
        channels: 1,
        delay: Duration(milliseconds: 40),
      ),
      spillDir: spill.path,
    ),
  );
}
