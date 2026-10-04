// Native build for the telosnex_audio code asset (ADR D2).
//
// Default: build from source with CMake into a build folder that persists in
// `outputDirectoryShared`, so a one-file edit recompiles one file (R1).
// A prebuilt from a GitHub release is used when its source hash matches.
//
// User defines (in the app's pubspec.yaml):
//   hooks:
//     user_defines:
//       telosnex_audio:
//         native: auto | source | prebuilt   # default auto
//         build_type: Release | RelWithDebInfo | Debug   # default Release
import 'dart:io';

import 'package:code_assets/code_assets.dart';
import 'package:crypto/crypto.dart';
import 'package:hooks/hooks.dart';

import 'prebuilt.g.dart';

const packageName = 'telosnex_audio';
const assetName = 'src/ffi.dart';

/// Targets with an engine. Other targets get no asset; the Dart API then
/// reports the engine as unavailable (ADR workplan step 6).
bool isSupportedTarget(OS os, Architecture arch) =>
    os == OS.macOS && (arch == Architecture.arm64 || arch == Architecture.x64);

Future<void> buildNative(BuildInput input, BuildOutputBuilder output) async {
  if (!input.config.buildCodeAssets) return;
  final code = input.config.code;
  final os = code.targetOS;
  final arch = code.targetArchitecture;
  final root = Directory.fromUri(input.packageRoot);
  final sources = nativeSourceFiles(root);
  output.dependencies.addAll(sources.map((f) => f.uri));
  if (!isSupportedTarget(os, arch)) return;

  final mode = (input.userDefines['native'] as String?) ?? 'auto';
  final buildType = (input.userDefines['build_type'] as String?) ?? 'Release';
  final libName = os.dylibFileName('telosnex_audio');
  final target = '${os.name}-${arch.name}';

  File? lib;
  if (mode != 'source') {
    lib = await _tryPrebuilt(input, target, libName, sources, root);
    if (lib == null && mode == 'prebuilt') {
      throw StateError('No prebuilt telosnex_audio for $target at this source hash.');
    }
  }
  lib ??= await _buildFromSource(input, root, os, arch, buildType, libName);

  final out = File.fromUri(input.outputDirectory.resolve(libName));
  await lib.copy(out.path);
  output.assets.code.add(
    CodeAsset(
      package: packageName,
      name: assetName,
      linkMode: DynamicLoadingBundled(),
      file: out.uri,
    ),
  );
}

/// Every file that affects the native build, in a stable order.
List<File> nativeSourceFiles(Directory root) {
  final files = <File>[
    File('${root.path}/CMakeLists.txt'),
  ];
  for (final dir in ['cmake', 'src', 'third_party']) {
    final d = Directory('${root.path}/$dir');
    if (!d.existsSync()) continue;
    files.addAll(d.listSync(recursive: true).whereType<File>().where((f) => !f.path.endsWith('.DS_Store')));
  }
  files.sort((a, b) => a.path.compareTo(b.path));
  return files;
}

/// SHA-256 over relative paths and contents of [nativeSourceFiles].
String nativeSourceHash(Directory root, List<File> files) {
  final sink = _DigestSink();
  final input = sha256.startChunkedConversion(sink);
  final prefix = root.absolute.path.length + 1;
  for (final f in files) {
    input.add(f.absolute.path.substring(prefix).replaceAll(r'\', '/').codeUnits);
    input.add([0]);
    input.add(f.readAsBytesSync());
    input.add([0]);
  }
  input.close();
  return sink.value.toString();
}

class _DigestSink implements Sink<Digest> {
  late Digest value;
  @override
  void add(Digest data) => value = data;
  @override
  void close() {}
}

Future<File?> _tryPrebuilt(
  BuildInput input,
  String target,
  String libName,
  List<File> sources,
  Directory root,
) async {
  final tag = prebuiltReleaseTag;
  final expected = prebuiltSha256['$target/$libName'];
  if (tag == null || expected == null) return null;
  if (nativeSourceHash(root, sources) != prebuiltSourceHash) return null;
  final dir = Directory.fromUri(input.outputDirectoryShared.resolve('prebuilt-${expected.substring(0, 12)}/'));
  final file = File('${dir.path}/$libName');
  if (file.existsSync() && sha256.convert(file.readAsBytesSync()).toString() == expected) {
    return file;
  }
  dir.createSync(recursive: true);
  final url = Uri.parse('https://github.com/Telosnex/telosnex_audio/releases/download/$tag/$target-$libName');
  final client = HttpClient();
  try {
    final req = await client.getUrl(url);
    final res = await req.close();
    if (res.statusCode != 200) return null;
    final bytes = <int>[];
    await for (final chunk in res) {
      bytes.addAll(chunk);
    }
    if (sha256.convert(bytes).toString() != expected) {
      throw StateError('Prebuilt $url has the wrong SHA-256.');
    }
    await file.writeAsBytes(bytes, flush: true);
    return file;
  } on SocketException {
    return null;
  } finally {
    client.close();
  }
}

Future<File> _buildFromSource(
  BuildInput input,
  Directory root,
  OS os,
  Architecture arch,
  String buildType,
  String libName,
) async {
  final code = input.config.code;
  final cmake = _findTool('cmake');
  if (cmake == null) {
    throw StateError('telosnex_audio needs CMake 3.22+ to build from source. Install it (brew install cmake ninja).');
  }
  final ninja = _findTool('ninja');
  final buildDir = Directory.fromUri(
    input.outputDirectoryShared.resolve('cmake-${os.name}-${arch.name}-$buildType/'),
  );
  final configureArgs = <String>[
    '-S', root.path,
    '-B', buildDir.path,
    '-DCMAKE_BUILD_TYPE=$buildType',
    if (ninja != null) ...['-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=$ninja'],
    if (os == OS.macOS) ...[
      '-DCMAKE_OSX_ARCHITECTURES=${arch == Architecture.arm64 ? 'arm64' : 'x86_64'}',
      '-DCMAKE_OSX_DEPLOYMENT_TARGET=${code.macOS.targetVersion}',
    ],
  ];
  final cache = File('${buildDir.path}/CMakeCache.txt');
  final stamp = File('${buildDir.path}/tsnx_configure_args.txt');
  final argsText = configureArgs.join('\n');
  if (!cache.existsSync() || !stamp.existsSync() || stamp.readAsStringSync() != argsText) {
    buildDir.createSync(recursive: true);
    await _run(cmake, configureArgs);
    stamp.writeAsStringSync(argsText);
  }
  await _run(cmake, [
    '--build', buildDir.path,
    '--target', 'telosnex_audio',
    '--parallel', '${Platform.numberOfProcessors}',
  ]);
  return File('${buildDir.path}/$libName');
}

String? _findTool(String name) {
  final exe = Platform.isWindows ? '$name.exe' : name;
  final path = Platform.environment['PATH'] ?? '';
  final dirs = [
    ...path.split(Platform.isWindows ? ';' : ':'),
    '/opt/homebrew/bin',
    '/usr/local/bin',
    '/Applications/CMake.app/Contents/bin',
  ];
  for (final d in dirs) {
    if (d.isEmpty) continue;
    final f = File('$d/$exe');
    if (f.existsSync()) return f.path;
  }
  return null;
}

Future<void> _run(String exe, List<String> args) async {
  final r = await Process.run(exe, args);
  if (r.exitCode != 0) {
    throw ProcessException(exe, args, '${r.stdout}\n${r.stderr}', r.exitCode);
  }
}
