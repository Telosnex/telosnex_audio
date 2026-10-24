/// Runs before each native build of the kiln release workflow: installs
/// Ninja, and on Linux also CMake, g++ and the ALSA and PulseAudio headers.
///
/// The workflow runs a copy of this file outside the package, so it imports
/// only dart: libraries.
library;

import 'dart:io';

Future<void> main() async {
  if (Platform.isLinux) {
    await _run('sudo', ['apt-get', 'update', '-q']);
    await _run('sudo', [
      'apt-get',
      'install',
      '-y',
      '-q',
      'cmake',
      'ninja-build',
      'g++',
      'libasound2-dev',
      'libpulse-dev',
    ]);
  } else if (Platform.isMacOS) {
    final ninja = await Process.run('which', ['ninja']);
    if (ninja.exitCode != 0) await _run('brew', ['install', 'ninja']);
  }
}

Future<void> _run(String executable, List<String> args) async {
  stdout.writeln('+ $executable ${args.join(' ')}');
  final process = await Process.start(
    executable,
    args,
    mode: ProcessStartMode.inheritStdio,
  );
  final code = await process.exitCode;
  if (code != 0) exit(code);
}
