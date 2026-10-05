// Bindings to the C API in src/tsnx_audio.h. The code asset id is this
// library's URI (see lib/src/hook/native_build.dart).
// ignore_for_file: non_constant_identifier_names
import 'dart:ffi';

import 'package:ffi/ffi.dart';

final class TsnxEngineConfig extends Struct {
  @Int32()
  external int manual_device;
  @Int32()
  external int manual_output_channels;
  @Int32()
  external int manual_delay_ms;
  @Int32()
  external int echo_cancellation;
  @Int32()
  external int noise_suppression;
  @Int32()
  external int auto_gain;
  @Int32()
  external int platform_voice_processing;
  @Int32()
  external int clock_correction;
  @Int32()
  external int linux_audio_backend;
  external Pointer<Utf8> spill_dir;
  external Pointer<NativeFunction<Void Function(Int32, Int32, Int64)>> notify;
}

final class TsnxTrackState extends Struct {
  @Int64()
  external int status;
  @Int64()
  external int position;
  @Int64()
  external int sampled_at_ns;
  @Double()
  external double rate;
  @Int64()
  external int written;
  @Int64()
  external int duration;
  @Int64()
  external int mixer_position;
  @Int64()
  external int underruns;
}

final class TsnxCaptureBlock extends Struct {
  @Int64()
  external int t_ns;
  @Int32()
  external int frames;
  @Int32()
  external int rate;
  @Array(480)
  external Array<Int16> data;
}

final class TsnxEngine extends Opaque {}

@Native<Pointer<Utf8> Function()>(symbol: 'tsnx_version', isLeaf: true)
external Pointer<Utf8> tsnxVersion();

@Native<
  Int32 Function(Pointer<TsnxEngineConfig>, Pointer<Pointer<TsnxEngine>>)
>(symbol: 'tsnx_engine_open')
external int tsnxEngineOpen(
  Pointer<TsnxEngineConfig> config,
  Pointer<Pointer<TsnxEngine>> out,
);

@Native<Void Function(Pointer<TsnxEngine>)>(symbol: 'tsnx_engine_close')
external void tsnxEngineClose(Pointer<TsnxEngine> e);

@Native<Int64 Function(Pointer<TsnxEngine>)>(
  symbol: 'tsnx_engine_now_ns',
  isLeaf: true,
)
external int tsnxEngineNowNs(Pointer<TsnxEngine> e);

@Native<
  Int32 Function(Pointer<TsnxEngine>, Int32, Pointer<Int16>, Pointer<Int16>)
>(symbol: 'tsnx_engine_manual_render')
external int tsnxEngineManualRender(
  Pointer<TsnxEngine> e,
  int blocks,
  Pointer<Int16> out,
  Pointer<Int16> captureIn,
);

@Native<Double Function(Pointer<TsnxEngine>)>(symbol: 'tsnx_engine_erle_db')
external double tsnxEngineErleDb(Pointer<TsnxEngine> e);

@Native<Int32 Function(Pointer<TsnxEngine>)>(
  symbol: 'tsnx_engine_output_delay_ms',
  isLeaf: true,
)
external int tsnxEngineOutputDelayMs(Pointer<TsnxEngine> e);

@Native<
  Int32 Function(Pointer<TsnxEngine>, Int32, Int32, Int32, Pointer<Int32>)
>(symbol: 'tsnx_track_create')
external int tsnxTrackCreate(
  Pointer<TsnxEngine> e,
  int rate,
  int channels,
  int retention,
  Pointer<Int32> outId,
);

@Native<
  Int32 Function(Pointer<TsnxEngine>, Pointer<Uint8>, Int64, Pointer<Int32>)
>(symbol: 'tsnx_track_create_mp3')
external int tsnxTrackCreateMp3(
  Pointer<TsnxEngine> e,
  Pointer<Uint8> data,
  int size,
  Pointer<Int32> outId,
);

@Native<Int64 Function(Pointer<TsnxEngine>, Int32, Pointer<Int16>, Int64)>(
  symbol: 'tsnx_track_write',
  isLeaf: true,
)
external int tsnxTrackWrite(
  Pointer<TsnxEngine> e,
  int id,
  Pointer<Int16> pcm,
  int frames,
);

@Native<Int32 Function(Pointer<TsnxEngine>, Int32)>(
  symbol: 'tsnx_track_end_of_stream',
  isLeaf: true,
)
external int tsnxTrackEndOfStream(Pointer<TsnxEngine> e, int id);

@Native<Int32 Function(Pointer<TsnxEngine>, Int32)>(
  symbol: 'tsnx_track_play',
  isLeaf: true,
)
external int tsnxTrackPlay(Pointer<TsnxEngine> e, int id);

@Native<Int32 Function(Pointer<TsnxEngine>, Int32)>(
  symbol: 'tsnx_track_pause',
  isLeaf: true,
)
external int tsnxTrackPause(Pointer<TsnxEngine> e, int id);

@Native<Int32 Function(Pointer<TsnxEngine>, Int32, Int64)>(
  symbol: 'tsnx_track_seek',
  isLeaf: true,
)
external int tsnxTrackSeek(Pointer<TsnxEngine> e, int id, int frame);

@Native<Int32 Function(Pointer<TsnxEngine>, Int32, Double)>(
  symbol: 'tsnx_track_set_rate',
  isLeaf: true,
)
external int tsnxTrackSetRate(Pointer<TsnxEngine> e, int id, double rate);

@Native<Int32 Function(Pointer<TsnxEngine>, Int32, Double, Int32)>(
  symbol: 'tsnx_track_set_gain',
  isLeaf: true,
)
external int tsnxTrackSetGain(
  Pointer<TsnxEngine> e,
  int id,
  double gain,
  int rampMs,
);

@Native<Int32 Function(Pointer<TsnxEngine>, Int32)>(
  symbol: 'tsnx_track_flush',
  isLeaf: true,
)
external int tsnxTrackFlush(Pointer<TsnxEngine> e, int id);

@Native<Int32 Function(Pointer<TsnxEngine>, Int32, Pointer<TsnxTrackState>)>(
  symbol: 'tsnx_track_get_state',
  isLeaf: true,
)
external int tsnxTrackGetState(
  Pointer<TsnxEngine> e,
  int id,
  Pointer<TsnxTrackState> out,
);

@Native<
  Int32 Function(Pointer<TsnxEngine>, Int32, Pointer<Int32>, Pointer<Int32>)
>(symbol: 'tsnx_track_format', isLeaf: true)
external int tsnxTrackFormat(
  Pointer<TsnxEngine> e,
  int id,
  Pointer<Int32> rate,
  Pointer<Int32> channels,
);

@Native<Int32 Function(Pointer<TsnxEngine>, Int32)>(
  symbol: 'tsnx_track_dispose',
)
external int tsnxTrackDispose(Pointer<TsnxEngine> e, int id);

@Native<Int32 Function(Pointer<TsnxEngine>, Int32, Int32)>(
  symbol: 'tsnx_capture_start',
)
external int tsnxCaptureStart(Pointer<TsnxEngine> e, int rate, int requestId);

@Native<Int32 Function(Pointer<TsnxEngine>, Int32)>(symbol: 'tsnx_capture_stop')
external int tsnxCaptureStop(Pointer<TsnxEngine> e, int requestId);

@Native<Int32 Function(Pointer<TsnxEngine>, Pointer<TsnxCaptureBlock>, Int32)>(
  symbol: 'tsnx_capture_read',
  isLeaf: true,
)
external int tsnxCaptureRead(
  Pointer<TsnxEngine> e,
  Pointer<TsnxCaptureBlock> out,
  int maxBlocks,
);

@Native<Int32 Function(Pointer<TsnxEngine>)>(
  symbol: 'tsnx_capture_dropped',
  isLeaf: true,
)
external int tsnxCaptureDropped(Pointer<TsnxEngine> e);

@Native<Int32 Function(Pointer<TsnxEngine>, Int32)>(symbol: 'tsnx_device_count')
external int tsnxDeviceCount(Pointer<TsnxEngine> e, int kind);

@Native<
  Int32 Function(
    Pointer<TsnxEngine>,
    Int32,
    Int32,
    Pointer<Utf8>,
    Int32,
    Pointer<Utf8>,
    Int32,
  )
>(symbol: 'tsnx_device_get')
external int tsnxDeviceGet(
  Pointer<TsnxEngine> e,
  int kind,
  int index,
  Pointer<Utf8> id,
  int idCap,
  Pointer<Utf8> name,
  int nameCap,
);

@Native<Int32 Function(Pointer<TsnxEngine>, Int32, Pointer<Utf8>, Int32)>(
  symbol: 'tsnx_device_select',
)
external int tsnxDeviceSelect(
  Pointer<TsnxEngine> e,
  int kind,
  Pointer<Utf8> id,
  int requestId,
);

@Native<
  Int32 Function(
    Pointer<TsnxEngine>,
    Int32,
    Pointer<Utf8>,
    Int32,
    Pointer<Utf8>,
    Int32,
    Pointer<Int32>,
  )
>(symbol: 'tsnx_device_current')
external int tsnxDeviceCurrent(
  Pointer<TsnxEngine> e,
  int kind,
  Pointer<Utf8> id,
  int idCap,
  Pointer<Utf8> name,
  int nameCap,
  Pointer<Int32> deviceKind,
);
