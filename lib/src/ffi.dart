// Bindings to the C API in src/tsnx_audio.h. The code asset id is this
// library's URI (see hook/build.dart).
import 'dart:ffi';

import 'package:ffi/ffi.dart';

@Native<Pointer<Utf8> Function()>(symbol: 'tsnx_version', isLeaf: true)
external Pointer<Utf8> tsnxVersion();
