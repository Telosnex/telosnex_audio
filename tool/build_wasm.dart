// Builds the web engine core (ADR D14) with Emscripten.
//
// Usage:
//   dart tool/build_wasm.dart          build lib/web/telosnex_audio.wasm
//   dart tool/build_wasm.dart --check  exit 1 if the sources changed since
//                                      the last build (no Emscripten needed)
//
// Hooks do not run for web builds, so the module is committed. The stamp
// file holds a hash of every input; CI runs --check.
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';

const _webrtc = 'third_party/webrtc';

const _cxxSources = [
  'src/web/web_core.cc',
  'src/track.cc',
  'src/track_store.cc',
  'src/block_resampler.cc',
  '$_webrtc/common_audio/resampler/sinc_resampler.cc',
  '$_webrtc/rtc_base/memory/aligned_malloc.cc',
];
const _cSources = [
  'src/third_party_impl/sonic_impl.c',
  'src/third_party_impl/minimp3_impl.c',
];

const _flags = [
  '-O3',
  '-DNDEBUG',
  '-DRTC_DISABLE_CHECK_MSG',
  '-Isrc',
  '-I$_webrtc',
  '-I$_webrtc/third_party/abseil-cpp',
  '-Ithird_party/sonic',
  '-Ithird_party/minimp3',
];
const _cxxFlags = ['-std=c++20', '-fno-exceptions'];
const _linkFlags = [
  '-O3',
  '-fno-exceptions',
  '-sSTANDALONE_WASM',
  '--no-entry',
  '-sALLOW_MEMORY_GROWTH=1',
  '-sFILESYSTEM=0',
];

const _out = 'lib/web/telosnex_audio.wasm';
const _stamp = 'lib/web/telosnex_audio.wasm.stamp';

Future<void> main(List<String> args) async {
  final root = File.fromUri(Platform.script).parent.parent.path;
  Directory.current = root;
  final hash = _inputHash();
  if (args.contains('--check')) {
    final stamp = File(_stamp);
    final recorded = stamp.existsSync()
        ? (jsonDecode(stamp.readAsStringSync()) as Map)['inputs']
        : null;
    if (recorded != hash) {
      stderr.writeln(
        '$_out is out of date with its sources. '
        'Run: dart tool/build_wasm.dart',
      );
      exit(1);
    }
    stdout.writeln('$_out is up to date.');
    return;
  }

  final tmp = Directory.systemTemp.createTempSync('tsnx_wasm');
  try {
    final objects = <String>[];
    final jobs = <Future<void>>[];
    for (final (i, src) in [..._cxxSources, ..._cSources].indexed) {
      final cxx = _cxxSources.contains(src);
      final obj = '${tmp.path}/$i.o';
      objects.add(obj);
      jobs.add(
        _run(cxx ? 'em++' : 'emcc', [
          ..._flags,
          if (cxx) ..._cxxFlags,
          '-c',
          src,
          '-o',
          obj,
        ]),
      );
    }
    await Future.wait(jobs);
    Directory('lib/web').createSync(recursive: true);
    await _run('em++', [...objects, ..._linkFlags, '-o', _out]);
    final version = (await Process.run('emcc', ['--version'])).stdout
        .toString()
        .split('\n')
        .first;
    File(_stamp).writeAsStringSync(
      '${const JsonEncoder.withIndent('  ').convert({'inputs': hash, 'emcc': version})}\n',
    );
    stdout.writeln('$_out: ${File(_out).lengthSync()} bytes');
  } finally {
    tmp.deleteSync(recursive: true);
  }
}

Future<void> _run(String exe, List<String> args) async {
  final r = await Process.run(exe, args);
  if (r.exitCode != 0) {
    stderr
      ..writeln('$exe ${args.join(' ')}')
      ..write(r.stdout)
      ..write(r.stderr);
    exit(r.exitCode);
  }
}

/// The sources, every header they can include from this package and the
/// two small vendored libraries, and the flags.
String _inputHash() {
  final files = <String>{
    ..._cxxSources,
    ..._cSources,
    for (final dir in ['src', 'third_party/sonic', 'third_party/minimp3'])
      for (final f in Directory(dir).listSync(recursive: true).whereType<File>())
        if (f.path.endsWith('.h')) f.path,
    '$_webrtc/common_audio/resampler/sinc_resampler.h',
    '$_webrtc/rtc_base/memory/aligned_malloc.h',
    '$_webrtc/rtc_base/checks.h',
    '$_webrtc/rtc_base/system/arch.h',
  }.toList()..sort();
  final all = BytesBuilder(copy: false);
  for (final f in files) {
    all
      ..add(utf8.encode('$f\n'))
      ..add(File(f).readAsBytesSync());
  }
  all.add(utf8.encode([..._flags, ..._cxxFlags, ..._linkFlags].join(' ')));
  return sha256.convert(all.takeBytes()).toString();
}
