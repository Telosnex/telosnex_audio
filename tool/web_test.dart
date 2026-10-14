// Runs the web tests on the web runtime files (native_prebuilt ADR 002 D10,
// D13): the engine cases in Chrome, then the real-time AudioWorklet check of
// the example app, compiled to JS and to Wasm.
//
//   dart tool/web_test.dart                  the released files of web.json
//   dart tool/web_test.dart --from <dir>     a tool/build_web.dart output
//
// With --from, the example app also uses <dir> (user defines web_build:
// local and web_build_dir), because the release for these sources does not
// exist yet. The script restores example/pubspec.yaml afterwards.
import 'dart:io';

Future<void> main(List<String> args) async {
  final String? from;
  if (args.isEmpty) {
    from = null;
  } else if (args.length == 2 && args.first == '--from') {
    from = Directory(args.last).absolute.path;
  } else {
    stderr.writeln('Usage: dart tool/web_test.dart [--from <dir>]');
    exit(2);
  }
  Directory.current = File.fromUri(Platform.script).parent.parent.path;

  // `dart run` cannot resolve the Flutter SDK dependency by itself.
  await _run('flutter', ['pub', 'get']);
  await _run('dart', [
    'run',
    'native_prebuilt:web',
    'install-dev',
    if (from != null) ...['--from', from],
  ]);
  await _run('flutter', [
    'test',
    '--platform',
    'chrome',
    'test/web_engine_test.dart',
    'test/web_runtime_files_test.dart',
  ]);

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
    for (final wasm in [false, true]) {
      await _run('flutter', [
        'build',
        'web',
        if (wasm) '--wasm',
        '-t',
        'lib/web_check.dart',
      ], cwd: 'example');
      await _run('dart', ['tool/web_check.dart', '--browser', 'chrome']);
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
    exit(code);
  }
}
