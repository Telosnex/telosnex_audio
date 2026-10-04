import 'package:hooks/hooks.dart';
import 'package:telosnex_audio/src/hook/native_build.dart';

void main(List<String> args) async {
  await build(args, buildNative);
}
