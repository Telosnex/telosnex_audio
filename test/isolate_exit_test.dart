// An engine can outlive the isolate that opened it: a Flutter hot restart
// ends the root isolate without running close(). The engine must not call
// into the dead isolate (crash: SIGBUS in Engine::NotifierPass), and the next
// open must close it so that one engine owns the device (ADR I9).
@TestOn('vm')
library;

import 'dart:async';
import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';

import 'package:flutter_test/flutter_test.dart';
import 'package:telosnex_audio/src/engine_native.dart';
import 'package:telosnex_audio/src/ffi.dart';
import 'package:telosnex_audio/telosnex_audio.dart';

Future<void> _openAndLeak((SendPort, String) args) async {
  final (reply, spillDir) = args;
  final engine = await TelosnexAudio.open(
    device: const ManualDevice(channels: 1, delay: Duration(milliseconds: 40)),
    spillDir: spillDir,
  );
  reply.send(nativeEngineHandle(engine).address);
}

/// Opens an engine and keeps it open until a message arrives on the port
/// that it sends to [reply]. Replies again after close.
Future<void> _openAndHold((SendPort, String) args) async {
  final (reply, spillDir) = args;
  final engine = await TelosnexAudio.open(
    device: const ManualDevice(channels: 1, delay: Duration(milliseconds: 40)),
    spillDir: spillDir,
  );
  final commands = ReceivePort();
  reply.send(commands.sendPort);
  await commands.first;
  await engine.close();
  commands.close();
  reply.send(null);
}

Future<AudioEngine> _openManual(String spillDir) => TelosnexAudio.open(
  device: const ManualDevice(channels: 1, delay: Duration(milliseconds: 40)),
  spillDir: spillDir,
);

/// The engine's spill directory. It exists while the engine is open.
Directory _engineDir(Directory spill) => Directory('${spill.path}/$pid');

void main() {
  late Directory spill;
  late Directory otherSpill;

  setUp(() {
    spill = Directory.systemTemp.createTempSync('tsnx_isolate_exit_test');
    otherSpill = Directory.systemTemp.createTempSync('tsnx_isolate_exit_test');
  });

  tearDown(() {
    for (final d in [spill, otherSpill]) {
      if (d.existsSync()) d.deleteSync(recursive: true);
    }
  });

  test('events of an engine whose isolate exited do not crash', () async {
    final reply = ReceivePort();
    final exited = ReceivePort();
    final isolate = await Isolate.spawn(_openAndLeak, (
      reply.sendPort,
      spill.path,
    ), onExit: exited.sendPort);
    final address = await reply.first as int;
    // The open engine keeps the isolate alive; a hot restart kills it.
    isolate.kill(priority: Isolate.immediate);
    await exited.first;

    final e = Pointer<TsnxEngine>.fromAddress(address);
    // Each request sends REQUEST_DONE to the owner, synchronously on the
    // manual device.
    for (var i = 1; i <= 20; i++) {
      expect(tsnxCaptureStart(e, 48000, i), 0);
      expect(tsnxCaptureStop(e, 100 + i), 0);
    }
    tsnxEngineClose(e);
  });

  test('open closes engines whose isolate exited', () async {
    final reply = ReceivePort();
    final exited = ReceivePort();
    final isolate = await Isolate.spawn(_openAndLeak, (
      reply.sendPort,
      spill.path,
    ), onExit: exited.sendPort);
    await reply.first;
    isolate.kill(priority: Isolate.immediate);
    await exited.first;
    expect(_engineDir(spill).existsSync(), isTrue);

    final engine = await _openManual(otherSpill.path);
    expect(_engineDir(spill).existsSync(), isFalse);
    await engine.close();
  });

  test('open keeps engines whose isolate is alive', () async {
    final reply = ReceivePort();
    final replies = StreamIterator(reply);
    await Isolate.spawn(_openAndHold, (reply.sendPort, spill.path));
    await replies.moveNext();
    final commands = replies.current as SendPort;

    final engine = await _openManual(otherSpill.path);
    expect(_engineDir(spill).existsSync(), isTrue);
    await engine.close();

    commands.send(null);
    await replies.moveNext();
    expect(_engineDir(spill).existsSync(), isFalse);
    reply.close();
  });
}
