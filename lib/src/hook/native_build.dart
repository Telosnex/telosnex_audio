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
    (os == OS.android && arch == Architecture.arm) ||
    (os == OS.macOS ||
            os == OS.linux ||
            os == OS.windows ||
            os == OS.iOS ||
            os == OS.android) &&
        (arch == Architecture.arm64 || arch == Architecture.x64);

String androidAbi(Architecture arch) => switch (arch) {
  Architecture.arm => 'armeabi-v7a',
  Architecture.arm64 => 'arm64-v8a',
  Architecture.x64 => 'x86_64',
  _ => throw UnsupportedError('Unsupported Android architecture: $arch'),
};

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
  final simulator =
      os == OS.iOS && code.iOS.targetSdk == IOSSdk.iPhoneSimulator;
  final target = '${simulator ? 'iossim' : os.name}-${arch.name}';

  // A desktop target on another host OS (flutterpi_tool builds Linux arm64
  // on macOS) has no compiler and sysroot here: use the prebuilt.
  final cross = os != OS.current && os != OS.iOS && os != OS.android;
  if (cross && mode == 'source') {
    throw StateError(
      'telosnex_audio cannot build $target from source on ${OS.current.name}. '
      'Build on a $target host, or use a prebuilt.',
    );
  }

  File? lib;
  if (mode != 'source') {
    lib = await _tryPrebuilt(input, target, libName, sources, root);
    if (lib == null && (mode == 'prebuilt' || cross)) {
      throw StateError(
        'No prebuilt telosnex_audio for $target at this source hash.',
      );
    }
  }
  lib ??= await _buildFromSource(
    input,
    root,
    os,
    arch,
    buildType,
    libName,
    target,
  );

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
  final files = <File>[File('${root.path}/CMakeLists.txt')];
  for (final dir in ['cmake', 'src', 'third_party']) {
    final d = Directory('${root.path}/$dir');
    if (!d.existsSync()) continue;
    files.addAll(
      d
          .listSync(recursive: true)
          .whereType<File>()
          .where((f) => !f.path.endsWith('.DS_Store')),
    );
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
    input.add(
      f.absolute.path.substring(prefix).replaceAll(r'\', '/').codeUnits,
    );
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
  final dir = Directory.fromUri(
    input.outputDirectoryShared.resolve(
      'prebuilt-${expected.substring(0, 12)}/',
    ),
  );
  final file = File('${dir.path}/$libName');
  if (file.existsSync() &&
      sha256.convert(file.readAsBytesSync()).toString() == expected) {
    return file;
  }
  dir.createSync(recursive: true);
  final url = Uri.parse(
    'https://github.com/Telosnex/telosnex_audio/releases/download/$tag/$target-$libName',
  );
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
  String target,
) async {
  final code = input.config.code;
  // Windows: CMake, Ninja, and cl.exe come from the Visual Studio developer
  // environment that the Flutter tool found.
  final env = os == OS.windows ? await _msvcEnvironment(code, arch) : null;
  final cmake = _findTool('cmake', env);
  if (cmake == null) {
    throw StateError(
      'telosnex_audio needs CMake 3.22+ to build from source. '
      'macOS: brew install cmake ninja. Linux: sudo apt install cmake ninja-build. '
      'Windows: the Visual Studio "C++ CMake tools" component.',
    );
  }
  final ninja = _findTool('ninja', env);
  final linuxCompilers = os == OS.linux ? _linuxCompilers(code) : null;
  final buildDir = Directory.fromUri(
    input.outputDirectoryShared.resolve('cmake-$target-$buildType/'),
  );
  final configureArgs = <String>[
    '-S',
    root.path,
    '-B',
    buildDir.path,
    '-DCMAKE_BUILD_TYPE=$buildType',
    if (ninja != null) ...['-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=$ninja'],
    if (os == OS.macOS) ...[
      '-DCMAKE_OSX_ARCHITECTURES=${arch == Architecture.arm64 ? 'arm64' : 'x86_64'}',
      '-DCMAKE_OSX_DEPLOYMENT_TARGET=${code.macOS.targetVersion}',
    ],
    if (os == OS.iOS) ...[
      '-DCMAKE_SYSTEM_NAME=iOS',
      '-DCMAKE_OSX_SYSROOT=${target.startsWith('iossim') ? 'iphonesimulator' : 'iphoneos'}',
      '-DCMAKE_OSX_ARCHITECTURES=${arch == Architecture.arm64 ? 'arm64' : 'x86_64'}',
      '-DCMAKE_OSX_DEPLOYMENT_TARGET=${code.iOS.targetVersion}',
    ],
    if (os == OS.android) ...[
      '-DCMAKE_TOOLCHAIN_FILE=${_androidNdk(code)}/build/cmake/android.toolchain.cmake',
      '-DANDROID_ABI=${androidAbi(arch)}',
      '-DANDROID_PLATFORM=android-${code.android.targetNdkApi}',
      '-DANDROID_STL=c++_static',
    ],
    if (linuxCompilers != null) ...[
      '-DCMAKE_C_COMPILER=${linuxCompilers.$1}',
      '-DCMAKE_CXX_COMPILER=${linuxCompilers.$2}',
    ],
    if (os == OS.windows) ...[
      '-DCMAKE_C_COMPILER=cl',
      '-DCMAKE_CXX_COMPILER=cl',
    ],
    if (os != OS.macOS && os != OS.iOS) '-DTSNX_TARGET_ARCH=${arch.name}',
  ];
  final cache = File('${buildDir.path}/CMakeCache.txt');
  final stamp = File('${buildDir.path}/tsnx_configure_args.txt');
  final argsText = configureArgs.join('\n');
  if (!cache.existsSync() ||
      !stamp.existsSync() ||
      stamp.readAsStringSync() != argsText) {
    buildDir.createSync(recursive: true);
    await _run(cmake, configureArgs, env);
    stamp.writeAsStringSync(argsText);
  }
  await _run(cmake, [
    '--build',
    buildDir.path,
    '--target',
    'telosnex_audio',
    '--parallel',
    '${Platform.numberOfProcessors}',
  ], env);
  return File('${buildDir.path}/$libName');
}

/// The NDK root, from the clang the Flutter tool gives
/// (`<ndk>/toolchains/llvm/prebuilt/<host>/bin/clang`).
String _androidNdk(CodeConfig code) {
  final cc = code.cCompiler?.compiler.toFilePath();
  if (cc == null) {
    throw StateError('telosnex_audio needs the Android NDK to build.');
  }
  var dir = File(cc).parent;
  for (var i = 0; i < 5; i++) {
    dir = dir.parent;
  }
  return dir.path;
}

/// The C and C++ compilers for Linux: the Flutter tool's clang when it gives
/// one, else CMake's default.
(String, String)? _linuxCompilers(CodeConfig code) {
  final cc = code.cCompiler?.compiler.toFilePath();
  if (cc == null) return null;
  final dir = File(cc).parent.path;
  final name = File(cc).uri.pathSegments.last;
  final cxx = name.contains('clang')
      ? name.replaceFirst('clang', 'clang++')
      : name.replaceFirst('gcc', 'g++');
  final cxxPath = '$dir/$cxx';
  if (!File(cxxPath).existsSync()) return null;
  return (cc, cxxPath);
}

/// Runs the Visual Studio developer prompt script and returns its
/// environment.
Future<Map<String, String>> _msvcEnvironment(
  CodeConfig code,
  Architecture arch,
) async {
  final prompt = code.cCompiler?.windows.developerCommandPrompt;
  if (prompt == null) {
    throw StateError(
      'telosnex_audio needs the Visual Studio C++ build tools on Windows.',
    );
  }
  final r = await Process.run('cmd', [
    '/c',
    prompt.script.toFilePath(),
    ...prompt.arguments,
    '&&',
    'set',
  ]);
  if (r.exitCode != 0) {
    throw ProcessException(
      prompt.script.toFilePath(),
      prompt.arguments,
      '${r.stdout}\n${r.stderr}',
      r.exitCode,
    );
  }
  final env = <String, String>{};
  for (final line in (r.stdout as String).split(RegExp(r'\r?\n'))) {
    final eq = line.indexOf('=');
    if (eq > 0) env[line.substring(0, eq)] = line.substring(eq + 1);
  }
  return env;
}

String? _findTool(String name, [Map<String, String>? env]) {
  final exe = Platform.isWindows ? '$name.exe' : name;
  final path =
      env?['Path'] ?? env?['PATH'] ?? Platform.environment['PATH'] ?? '';
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

Future<void> _run(
  String exe,
  List<String> args, [
  Map<String, String>? env,
]) async {
  final r = await Process.run(
    exe,
    args,
    environment: env,
    includeParentEnvironment: env == null,
  );
  if (r.exitCode != 0) {
    throw ProcessException(exe, args, '${r.stdout}\n${r.stderr}', r.exitCode);
  }
}
