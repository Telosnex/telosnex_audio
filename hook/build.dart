import 'package:hooks/hooks.dart';

import 'native_build.dart';

void main(List<String> args) async {
  await build(args, buildNative);
}
