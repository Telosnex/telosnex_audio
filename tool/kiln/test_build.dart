/// Runs after each native build of the kiln release workflow:
///
/// - Linux: the newest glibc requirement of the library is 2.35 or older.
/// - Android arm64 and x64: every LOAD segment is 16 KB aligned.
///
/// The workflow runs a copy of this file outside the package, so it imports
/// only dart: libraries.
library;

import 'dart:io';

const _glibcBaseline = [2, 35];

Future<void> main() async {
  final env = Platform.environment;
  final target = env['TARGET']!;
  final lib = '${env['OUT']}/out/$target/libtelosnex_audio.so';

  if (target.startsWith('linux-')) {
    final symbols = await _run('objdump', ['-T', lib]);
    final versions = {
      for (final m in RegExp(r'GLIBC_([0-9.]+)').allMatches(symbols))
        m[1]!.split('.').map(int.parse).toList(),
    }.toList()..sort(_compare);
    final newest = versions.last;
    stdout.writeln('Newest glibc requirement: ${newest.join('.')}');
    if (_compare(newest, _glibcBaseline) > 0) {
      _fail('the library needs glibc ${newest.join('.')}, newer than 2.35.');
    }
  } else if (target == 'android-arm64' || target == 'android-x64') {
    final readelf =
        '${env['ANDROID_NDK_HOME']}/toolchains/llvm/prebuilt/linux-x86_64/'
        'bin/llvm-readelf';
    final headers = await _run(readelf, ['-lW', lib]);
    final alignments = [
      for (final line in headers.split('\n'))
        if (line.trim().startsWith('LOAD'))
          line.trim().split(RegExp(r'\s+')).last,
    ];
    stdout.writeln('LOAD alignments: ${alignments.join(' ')}');
    if (alignments.isEmpty || alignments.any((a) => a != '0x4000')) {
      _fail('a LOAD segment is not 16 KB aligned.');
    }
  }
}

int _compare(List<int> a, List<int> b) {
  for (var i = 0; i < a.length && i < b.length; i++) {
    if (a[i] != b[i]) return a[i].compareTo(b[i]);
  }
  return a.length.compareTo(b.length);
}

Future<String> _run(String executable, List<String> args) async {
  final result = await Process.run(executable, args);
  if (result.exitCode != 0) {
    _fail('$executable ${args.join(' ')}: ${result.stderr}');
  }
  return '${result.stdout}';
}

Never _fail(String message) {
  stderr.writeln('test_build: $message');
  exit(1);
}
