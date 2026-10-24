/// Builds the web engine core (ADR D14) with Emscripten: telosnex_audio.wasm
/// in --out.
///
/// The kiln release workflow runs it in the pinned emscripten/emsdk:5.0.0
/// image (kiln ADR 002 D10). --toolchain-file gets the Emscripten version
/// for web.json. For a local build, activate Emscripten
/// 5.0.0 or put it at ../emsdk, then install the file into lib/web/ for the
/// tests and the example app:
///
///     dart tool/kiln/build_web.dart --out /tmp/telosnex_audio_web
///     dart run kiln:web install-dev --from /tmp/telosnex_audio_web
///
/// Without --from, install-dev downloads the released file of
/// native_artifacts/web.json.
library;

// It imports no package, so it runs without pub get (`dart tool/...`) in the
// Emscripten image, which has no Flutter SDK.
import 'dart:io';

const _usage =
    'dart run tool/kiln/build_web.dart --out <dir> [--toolchain-file <file>]\n'
    '  --out             Directory for telosnex_audio.wasm.\n'
    '  --toolchain-file  Gets the Emscripten version.';

const _output = 'telosnex_audio.wasm';
const _emscripten = '5.0.0 (a7c5deabd7c88ba1c38ebe988112256775f944c6)';

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

Future<void> main(List<String> args) async {
  final (outPath, toolchainFile) = _parse(args);
  final out = Directory(outPath).absolute;
  final root = Directory.fromUri(
    Platform.script.resolve('../..'),
  ).absolute.path;
  if (await out.exists()) {
    await for (final entity in out.list()) {
      final name = entity.uri.pathSegments.lastWhere((s) => s.isNotEmpty);
      if (name != _output) _fail('--out holds other files: $name');
    }
  }
  await out.create(recursive: true);

  final emsdkEnv = await _emsdkEnvironment(root);
  final version = await _firstLine(['emcc', '--version'], root, emsdkEnv);
  if (!version.contains(_emscripten)) {
    _fail('Emscripten $_emscripten is required, found: $version');
  }
  if (toolchainFile != null) {
    await File(toolchainFile).writeAsString('Emscripten $_emscripten\n');
  }

  final tmp = await Directory.systemTemp.createTemp('tsnx_wasm');
  try {
    final objects = <String>[];
    final jobs = <Future<void>>[];
    for (final (i, src) in [..._cxxSources, ..._cSources].indexed) {
      final cxx = _cxxSources.contains(src);
      final obj = '${tmp.path}/$i.o';
      objects.add(obj);
      jobs.add(
        _run(
          [
            cxx ? 'em++' : 'emcc',
            ..._flags,
            if (cxx) ..._cxxFlags,
            '-c',
            src,
            '-o',
            obj,
          ],
          root,
          emsdkEnv,
        ),
      );
    }
    await Future.wait(jobs);
    final wasm = File('${out.path}/$_output');
    await _run(
      ['em++', ...objects, ..._linkFlags, '-o', wasm.path],
      root,
      emsdkEnv,
    );
    stdout.writeln('Built $_output: ${await wasm.length()} bytes');
  } finally {
    await tmp.delete(recursive: true);
  }
}

(String, String?) _parse(List<String> args) {
  if (args.contains('-h') || args.contains('--help')) {
    stdout.writeln(_usage);
    exit(0);
  }
  return switch (args) {
    ['--out', final out] => (out, null),
    ['--out', final out, '--toolchain-file', final file] => (out, file),
    _ => _fail(_usage),
  };
}

/// Null if the emcc on the PATH is the pinned version, else
/// ../emsdk/emsdk_env.sh, which each command then sources.
Future<String?> _emsdkEnvironment(String root) async {
  try {
    final probe = await Process.run('emcc', ['--version']);
    if (probe.exitCode == 0 && '${probe.stdout}'.contains(_emscripten)) {
      return null;
    }
  } on ProcessException {
    // Not on the PATH.
  }
  final env = File('$root/../emsdk/emsdk_env.sh');
  if (env.existsSync()) return env.absolute.path;
  _fail(
    'Emscripten $_emscripten not found. Activate it, or put it at '
    '${env.parent.path}.',
  );
}

List<String> _wrap(List<String> command, String? emsdkEnv) => emsdkEnv == null
    ? command
    : [
        'bash',
        '-c',
        'source "\$0" > /dev/null && exec "\$@"',
        emsdkEnv,
        ...command,
      ];

/// The first line of the output of [command].
Future<String> _firstLine(
  List<String> command,
  String cwd,
  String? emsdkEnv,
) async {
  final c = _wrap(command, emsdkEnv);
  final result = await Process.run(
    c.first,
    c.skip(1).toList(),
    workingDirectory: cwd,
    environment: {if (emsdkEnv != null) 'EMSDK_QUIET': '1'},
  );
  if (result.exitCode != 0) {
    _fail('${command.join(' ')} failed: ${result.stderr}');
  }
  return '${result.stdout}'.split('\n').first;
}

Future<void> _run(List<String> command, String cwd, String? emsdkEnv) async {
  final c = _wrap(command, emsdkEnv);
  final result = await Process.run(
    c.first,
    c.skip(1).toList(),
    workingDirectory: cwd,
    environment: {if (emsdkEnv != null) 'EMSDK_QUIET': '1'},
  );
  if (result.exitCode != 0) {
    _fail(
      '${command.join(' ')}\n${result.stdout}${result.stderr}'
      'exit code ${result.exitCode}',
    );
  }
}

Never _fail(String message) {
  stderr.writeln('build_web: $message');
  exit(1);
}
