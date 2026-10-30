// Native build for the telosnex_audio code asset (ADR D2).
//
// package:kiln (kiln
// docs/ADR_001_PREBUILT_NATIVE_LIBRARIES.md) chooses between the prebuilt
// files in native_artifacts/prebuilt.json and a source build. The source
// build uses CMake with a build folder that persists in
// `outputDirectoryShared`, so a one-file edit recompiles one file (R1).
//
// User defines (in the app's pubspec.yaml):
//   hooks:
//     user_defines:
//       telosnex_audio:
//         native_build: auto | download | source   # default auto
//         build_type: Release | RelWithDebInfo | Debug   # default Release;
//                                                        # needs source
//         android_ndk_home: <path>   # NDK for an Android source build when
//                                    # the Flutter tool gives no compiler
import 'dart:io';

import 'package:code_assets/code_assets.dart';
import 'package:hooks/hooks.dart';
import 'package:kiln/kiln.dart';

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

/// The `vcvarsall.bat` argument for a build on [host] for [target]:
/// `x64`, `arm64`, or a cross pair such as `x64_arm64`.
String vcvarsArch(Architecture host, Architecture target) {
  String name(Architecture a) => switch (a) {
    Architecture.x64 => 'x64',
    Architecture.arm64 => 'arm64',
    _ => throw UnsupportedError('No MSVC toolset for $a'),
  };
  return host == target ? name(target) : '${name(host)}_${name(target)}';
}

Future<void> buildNative(BuildInput input, BuildOutputBuilder output) async {
  if (!input.config.buildCodeAssets) return;
  final code = input.config.code;
  if (!isSupportedTarget(code.targetOS, code.targetArchitecture)) return;

  final buildType = (input.userDefines['build_type'] as String?) ?? 'Release';
  if (buildType != 'Release' && input.userDefines['native_build'] != 'source') {
    throw StateError(
      'telosnex_audio: build_type $buildType needs native_build: source. '
      'Prebuilt files are Release builds.',
    );
  }
  await Kiln(
    input: input,
    output: output,
    log: (message) => stderr.writeln('[telosnex_audio] $message'),
  ).run((source) => _publishSourceBuild(input, output, source, buildType));
}

Future<void> _publishSourceBuild(
  BuildInput input,
  BuildOutputBuilder output,
  SourceBuild source,
  String buildType,
) async {
  final code = input.config.code;
  final os = code.targetOS;
  final arch = code.targetArchitecture;
  final libName = os.dylibFileName('telosnex_audio');
  final simulator =
      os == OS.iOS && code.iOS.targetSdk == IOSSdk.iPhoneSimulator;
  final target = '${simulator ? 'iossim' : os.name}-${arch.name}';

  // A desktop target on another host OS (flutterpi_tool builds Linux arm64
  // on macOS) has no compiler and sysroot here.
  if (os != OS.current && os != OS.iOS && os != OS.android) {
    throw StateError(
      'telosnex_audio cannot build $target from source on '
      '${OS.current.name}, and prebuilt files were not used because '
      '${source.reason}. Build on a $target host, or run the native release '
      'workflow of telosnex_audio for these sources.',
    );
  }

  final lib = await _buildFromSource(
    input,
    Directory.fromUri(input.packageRoot),
    os,
    arch,
    buildType,
    libName,
    target,
  );
  final out = File.fromUri(input.outputDirectory.resolve(libName));
  await Directory.fromUri(input.outputDirectory).create(recursive: true);
  await lib.copy(out.path);
  // Release files have no debug symbols. Strip only the published copy, so
  // the incremental build folder stays valid.
  if (source.release != null) {
    await _strip(out, os, ndk: os == OS.android ? _androidNdk(input) : null);
  }
  output.assets.code.add(
    CodeAsset(
      package: packageName,
      name: assetName,
      linkMode: DynamicLoadingBundled(),
      file: out.uri,
    ),
  );
}

Future<void> _strip(File library, OS os, {String? ndk}) async {
  switch (os) {
    case OS.macOS || OS.iOS:
      await _run('strip', ['-x', library.path]);
    case OS.linux:
      await _run('strip', ['--strip-unneeded', library.path]);
    case OS.android:
      await _run(_androidLlvmTool(ndk!, 'llvm-strip'), [
        '--strip-unneeded',
        library.path,
      ]);
    default:
      // MSVC Release DLLs keep their symbols in a separate PDB.
      break;
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
  // environment.
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
      '-DCMAKE_TOOLCHAIN_FILE=${_androidNdk(input)}/build/cmake/android.toolchain.cmake',
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

/// The NDK root. A Flutter build gives its clang
/// (`<ndk>/toolchains/llvm/prebuilt/<host>/bin/clang`). `kiln:build`
/// gives no compiler, so then the `android_ndk_home` user define or the
/// environment names the NDK.
String _androidNdk(BuildInput input) {
  final cc = input.config.code.cCompiler?.compiler.toFilePath();
  if (cc != null) {
    var dir = File(cc).parent;
    for (var i = 0; i < 5; i++) {
      dir = dir.parent;
    }
    return dir.path;
  }
  final env = Platform.environment;
  final ndk =
      input.userDefines.path('android_ndk_home')?.toFilePath() ??
      env['ANDROID_NDK_HOME'] ??
      env['ANDROID_NDK_ROOT'];
  if (ndk == null || !File('$ndk/source.properties').existsSync()) {
    throw StateError(
      'telosnex_audio needs the Android NDK to build. Set the '
      'android_ndk_home user define or ANDROID_NDK_HOME to the NDK directory.',
    );
  }
  return ndk;
}

/// An LLVM tool of the NDK at [ndk], such as `llvm-strip`.
String _androidLlvmTool(String ndk, String name) {
  final prebuilt = Directory('$ndk/toolchains/llvm/prebuilt');
  for (final host in prebuilt.listSync().whereType<Directory>()) {
    final exe = '${host.path}/bin/$name${Platform.isWindows ? '.exe' : ''}';
    if (File(exe).existsSync()) return exe;
  }
  throw StateError('The NDK at $ndk has no $name.');
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
/// environment. The Flutter tool gives the script. `kiln:build`
/// does not, so then vswhere finds `vcvarsall.bat`.
Future<Map<String, String>> _msvcEnvironment(
  CodeConfig code,
  Architecture arch,
) async {
  final prompt = code.cCompiler?.windows.developerCommandPrompt;
  final String script;
  final List<String> arguments;
  if (prompt != null) {
    script = prompt.script.toFilePath();
    arguments = prompt.arguments;
  } else {
    script = await _vcvarsall();
    arguments = [vcvarsArch(Architecture.current, arch)];
  }
  final r = await Process.run('cmd', ['/c', script, ...arguments, '&&', 'set']);
  if (r.exitCode != 0) {
    throw ProcessException(
      script,
      arguments,
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

Future<String> _vcvarsall() async {
  final programFiles =
      Platform.environment['ProgramFiles(x86)'] ?? r'C:\Program Files (x86)';
  final vswhere =
      '$programFiles\\Microsoft Visual Studio\\Installer\\vswhere.exe';
  if (File(vswhere).existsSync()) {
    final r = await Process.run(vswhere, [
      '-latest',
      '-products',
      '*',
      '-requires',
      'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
      '-property',
      'installationPath',
    ]);
    final path = (r.stdout as String).trim().split(RegExp(r'\r?\n')).first;
    final script = '$path\\VC\\Auxiliary\\Build\\vcvarsall.bat';
    if (path.isNotEmpty && File(script).existsSync()) return script;
  }
  throw StateError(
    'telosnex_audio needs the Visual Studio C++ build tools on Windows.',
  );
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
