// native_prebuilt ADR 002 D7 and I3: every web runtime file loads with the
// query v=<webRuntimeVersion>, so a browser does not use a cached file of
// another version.
@TestOn('browser')
library;

import 'dart:js_interop';

import 'package:flutter_test/flutter_test.dart';
import 'package:telosnex_audio/src/web_runtime.g.dart';
import 'package:telosnex_audio/telosnex_audio.dart';
import 'package:telosnex_audio/web_testing.dart';
import 'package:web/web.dart' as web;

const _files = {
  'telosnex_audio.wasm',
  'telosnex_audio_storage.js',
  'telosnex_audio_worklet.js',
};

void main() {
  test('an app loads the files from native_prebuilt/telosnex_audio/', () {
    for (final file in _files) {
      expect(
        webRuntimeUrl(file),
        'native_prebuilt/telosnex_audio/$file?v=$webRuntimeVersion',
      );
    }
  });

  test('the engine loads every file with the webVersion query', () async {
    // The test runner's scripts fill the 250-entry buffer.
    web.window.performance
      ..clearResourceTimings()
      ..setResourceTimingBufferSize(1000);
    // ManualDevice loads the worklet file with a script tag. Chrome does not
    // record AudioWorklet modules as resources; both use webRuntimeUrl.
    final engine = await TelosnexAudio.open(
      device: const ManualDevice(channels: 1, delay: Duration.zero),
    );
    addTearDown(engine.close);
    // The Worker script is recorded when it has loaded, which can be later.
    List<Uri> loaded() => [
      for (final entry
          in web.window.performance.getEntriesByType('resource').toDart)
        Uri.parse(entry.name),
    ].where((u) => _files.contains(u.pathSegments.last)).toList();
    final deadline = DateTime.now().add(const Duration(seconds: 10));
    // In a test the wasm loads twice: a 404 at the app path, then the test
    // path.
    while (loaded().map((u) => u.pathSegments.last).toSet().length <
            _files.length &&
        DateTime.now().isBefore(deadline)) {
      await Future<void>.delayed(const Duration(milliseconds: 20));
    }
    expect(loaded().map((u) => u.pathSegments.last).toSet(), _files);
    for (final u in loaded()) {
      expect(u.queryParameters['v'], webRuntimeVersion, reason: '$u');
    }
  });
}
