// The shared engine cases in a browser: the wasm core on the main thread
// (ManualDevice) and the storage worker.
@TestOn('browser')
library;

import 'dart:js_interop';

import 'package:flutter_test/flutter_test.dart';
import 'package:telosnex_audio/telosnex_audio.dart';
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
}
