// Builds the top-level LICENSE that Flutter includes in its license registry.
// Usage: dart tool/licenses.dart [--check]
// Keep upstream license text unchanged. Each section uses Flutter's package
// names + blank line + text format, separated by 80 hyphens.
import 'dart:io';

const licenseSources = <String, String>{
  'third_party/webrtc/LICENSE': 'WebRTC',
  'third_party/webrtc/PATENTS': 'WebRTC (patent grant)',
  'third_party/webrtc/NOTICE': 'WebRTC (Shiguredo modifications)',
  'third_party/webrtc/third_party/abseil-cpp/LICENSE': 'Abseil',
  'third_party/webrtc/third_party/pffft/LICENSE': 'PFFFT / FFTPACK',
  'third_party/webrtc/third_party/rnnoise/COPYING': 'RNNoise',
  'third_party/webrtc/modules/third_party/fft/LICENSE': 'FFT (Mark Olesen)',
  'third_party/webrtc/modules/third_party/portaudio/LICENSE': 'PortAudio',
  'third_party/webrtc/common_audio/third_party/spl_sqrt_floor/LICENSE':
      'SPL square root (Wilco Dijkstra)',
  'third_party/webrtc/common_audio/third_party/ooura/LICENSE': 'Ooura FFT',
  'third_party/sonic/LICENSE': 'Sonic',
  'third_party/minimp3/LICENSE': 'minimp3',
};

String bundledLicenses(Directory root) {
  // Fail if vendoring adds an unlisted license. New components need a named
  // section, not a silent omission from the app's license page.
  const names = {
    'LICENSE',
    'LICENSE.txt',
    'LICENSE.md',
    'COPYING',
    'PATENTS',
    'NOTICE',
  };
  final found = Directory('${root.path}/third_party')
      .listSync(recursive: true, followLinks: false)
      .whereType<File>()
      .where((f) => names.contains(f.uri.pathSegments.last))
      .map((f) => f.path.substring(root.path.length + 1).replaceAll('\\', '/'))
      .toSet();
  final missing = found.difference(licenseSources.keys.toSet());
  if (missing.isNotEmpty) {
    throw StateError('Unlisted upstream notices: ${missing.join(', ')}');
  }
  final sections = <String>[];
  for (final entry in licenseSources.entries) {
    var text = File('${root.path}/${entry.key}').readAsStringSync();
    // Sonic's license file has only the Apache terms. Its source headers
    // contain the author's copyright notice, which belongs here too.
    if (entry.value == 'Sonic') {
      text = 'Sonic library\nCopyright 2010 Bill Cox\n\n$text';
    }
    sections.add('telosnex_audio\n${entry.value}\n\n$text');
  }
  return sections.join('\n${'-' * 80}\n');
}

void main(List<String> args) {
  if (args.isNotEmpty && (args.length != 1 || args.single != '--check')) {
    stderr.writeln('Usage: dart tool/licenses.dart [--check]');
    exitCode = 64;
    return;
  }
  final root = File.fromUri(Platform.script).parent.parent;
  final license = File('${root.path}/LICENSE');
  final expected = bundledLicenses(root);
  if (args.contains('--check')) {
    if (!license.existsSync() || license.readAsStringSync() != expected) {
      stderr.writeln('LICENSE is stale. Run dart tool/licenses.dart.');
      exitCode = 1;
    }
  } else {
    license.writeAsStringSync(expected);
  }
}
