import 'package:code_assets/code_assets.dart';
import 'package:hooks/hooks.dart';
import 'package:native_prebuilt/native_prebuilt.dart';

import 'native_build.dart';

void main(List<String> args) async {
  await build(args, (input, output) async {
    if (!input.config.buildCodeAssets) {
      // A web build: install the web runtime files of native_artifacts/
      // web.json into the app (native_prebuilt ADR 002).
      return NativePrebuilt(input: input, output: output).installWeb();
    }
    await buildNative(input, output);
  });
}
