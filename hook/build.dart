import 'package:code_assets/code_assets.dart';
import 'package:hooks/hooks.dart';
import 'package:kiln/kiln.dart';

import '../hook_src/native_build.dart';

void main(List<String> args) async {
  await build(args, (input, output) async {
    if (!input.config.buildCodeAssets) {
      // A web build: install the web runtime files of native_artifacts/
      // web.json into the app (kiln ADR 002).
      return Kiln(input: input, output: output).installWeb();
    }
    await buildNative(input, output);
  });
}
