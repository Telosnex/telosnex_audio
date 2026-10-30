import 'package:code_assets/code_assets.dart';
import 'package:test/test.dart';

import '../hook_src/native_build.dart';

void main() {
  test(
    'the Android hook supports ARM32, ARM64 and x64 with the correct ABI',
    () {
      expect(isSupportedTarget(OS.android, Architecture.arm), isTrue);
      expect(isSupportedTarget(OS.android, Architecture.arm64), isTrue);
      expect(isSupportedTarget(OS.android, Architecture.x64), isTrue);
      expect(androidAbi(Architecture.arm), 'armeabi-v7a');
      expect(androidAbi(Architecture.arm64), 'arm64-v8a');
      expect(androidAbi(Architecture.x64), 'x86_64');
    },
  );

  test('ARM32 is Android-only and unsupported ABIs fail explicitly', () {
    for (final os in [OS.macOS, OS.iOS, OS.linux, OS.windows]) {
      expect(isSupportedTarget(os, Architecture.arm), isFalse);
    }
    expect(isSupportedTarget(OS.android, Architecture.ia32), isFalse);
    expect(() => androidAbi(Architecture.ia32), throwsUnsupportedError);
  });

  test('MSVC cross toolsets are named host_target', () {
    expect(vcvarsArch(Architecture.x64, Architecture.x64), 'x64');
    expect(vcvarsArch(Architecture.arm64, Architecture.arm64), 'arm64');
    expect(vcvarsArch(Architecture.x64, Architecture.arm64), 'x64_arm64');
    expect(vcvarsArch(Architecture.arm64, Architecture.x64), 'arm64_x64');
  });
}
