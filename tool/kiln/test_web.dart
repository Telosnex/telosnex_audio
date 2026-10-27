// Runs the web tests on the web runtime files (kiln ADR 002 D10,
// D13): the engine cases in Chrome, then the real-time AudioWorklet check of
// the example app, compiled to JS and to Wasm.
//
//   dart tool/kiln/test_web.dart               the released files of web.json
//   dart tool/kiln/test_web.dart --from <dir>  a tool/kiln/build_web.dart output
//   ... --browser firefox                      the AudioWorklet check in
//                                              headless Firefox, JS only
//   ... --browser safari                       the same in Safari (through
//                                              safaridriver), JS only
//
// With --from, the example app also uses <dir> (user defines web_build:
// local and web_build_dir), because the release for these sources does not
// exist yet. The script restores example/pubspec.yaml afterwards. The kiln
// release workflow runs it with KILN_WEB_FILES, the same as --from.
import 'dart:io';

Future<void> main(List<String> args) async {
  try {
    await _main(args);
  } on _Failed catch (e) {
    exit(e.code);
  }
}

Future<void> _main(List<String> args) async {
  String? from;
  var browser = 'chrome';
  for (var i = 0; i < args.length; i++) {
    if (args[i] == '--from' && i + 1 < args.length) {
      from = Directory(args[++i]).absolute.path;
    } else if (args[i] == '--browser' &&
        i + 1 < args.length &&
        const {'chrome', 'firefox', 'safari'}.contains(args[i + 1])) {
      browser = args[++i];
    } else {
      stderr.writeln(
        'Usage: dart tool/kiln/test_web.dart [--from <dir>] '
        '[--browser chrome|firefox|safari]',
      );
      exit(2);
    }
  }
  from ??= switch (Platform.environment['KILN_WEB_FILES']) {
    final String dir when dir.isNotEmpty => Directory(dir).absolute.path,
    _ => null,
  };
  Directory.current = File.fromUri(Platform.script).parent.parent.parent.path;

  // `dart run` cannot resolve the Flutter SDK dependency by itself.
  await _run('flutter', ['pub', 'get']);
  await _run('dart', [
    'run',
    'kiln:web',
    'install-dev',
    if (from != null) ...['--from', from],
  ]);
  // flutter test runs browser tests only in Chrome.
  if (browser == 'chrome') {
    await _run('flutter', [
      'test',
      '--platform',
      'chrome',
      'test/web_engine_test.dart',
      'test/web_runtime_files_test.dart',
    ]);
  }

  final pubspec = File('example/pubspec.yaml');
  final original = await pubspec.readAsString();
  try {
    if (from != null) {
      await pubspec.writeAsString(
        '$original\n'
        'hooks:\n'
        '  user_defines:\n'
        '    telosnex_audio:\n'
        '      web_build: local\n'
        '      web_build_dir: $from\n',
      );
    }
    await _run('flutter', ['pub', 'get'], cwd: 'example');
    // Flutter runs dart2wasm builds only in Chromium (no wasmAllowList).
    for (final wasm in [false, if (browser == 'chrome') true]) {
      await _run('flutter', [
        'build',
        'web',
        if (wasm) '--wasm',
        '-t',
        'lib/web_check.dart',
      ], cwd: 'example');
      await _run('dart', ['tool/web_check.dart', '--browser', browser]);
    }
  } finally {
    if (from != null) {
      await pubspec.writeAsString(original);
      await _run('flutter', ['pub', 'get'], cwd: 'example');
    }
  }
}

Future<void> _run(String exe, List<String> args, {String? cwd}) async {
  stdout.writeln('+ $exe ${args.join(' ')}');
  final process = await Process.start(
    exe,
    args,
    workingDirectory: cwd,
    runInShell: Platform.isWindows,
    mode: ProcessStartMode.inheritStdio,
  );
  final code = await process.exitCode;
  if (code != 0) {
    stderr.writeln('$exe ${args.first} failed with exit code $code.');
    // Thrown, not exit(): the finally in main restores example/pubspec.yaml.
    throw _Failed(code);
  }
}

final class _Failed implements Exception {
  _Failed(this.code);
  final int code;
}
