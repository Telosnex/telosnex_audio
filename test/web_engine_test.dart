// The shared engine cases in a browser: the wasm core on the main thread
// (ManualDevice) and the storage worker.
@TestOn('browser')
library;

import 'dart:js_interop';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:telosnex_audio/telosnex_audio.dart';
import 'package:telosnex_audio/web_testing.dart';
import 'package:web/web.dart' as web;

import 'engine_cases.dart';

void main() {
  engineCases(
    readFixture: (name) async {
      // test/fixtures links to native_test/fixtures; the test server
      // serves test/ at its root.
      final r = await web.window.fetch('fixtures/$name'.toJS).toDart;
      if (!r.ok) throw StateError('fixtures/$name: HTTP ${r.status}');
      return (await r.arrayBuffer().toDart).toDart.asUint8List();
    },
    () => TelosnexAudio.open(
      device: const ManualDevice(
        channels: 1,
        delay: Duration(milliseconds: 40),
      ),
    ),
  );

  // Without a user gesture the AudioContext stays suspended; commands still
  // reach the core and show in the state.
  test('a suspended context applies commands without rendering', () async {
    final e = await TelosnexAudio.open();
    addTearDown(e.close);
    expect(debugContextState(e), 'suspended');
    final t = e.createTrack(const PcmFormat(sampleRate: 24000, channels: 1))
      ..write(Int16List(24000 * 3))
      ..seek(const Duration(seconds: 2));
    await Future<void>.delayed(const Duration(milliseconds: 200));
    expect(t.state.position, const Duration(seconds: 2));
    expect(t.state.writtenFrames, 24000 * 3);
  });
}
