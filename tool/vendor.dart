// Copies the pinned WebRTC, Sonic, and minimp3 sources into third_party/.
//
// Usage:
//   dart tool/vendor.dart [--webrtc-src <gclient src dir>] [--gen-dir <dir>]
//
// The WebRTC files come from git objects at the pinned commits, not from the
// working tree, so local patches in the checkout do not leak in. Headers are
// found by following `#include "..."` lines from the listed sources.
//
// Local changes to vendored WebRTC files are normal commits (for example
// the ALSA patches from the fork). A run of this tool reverts them in the
// working tree; restore them before you commit.
//
// Outputs (all regenerated; do not edit by hand):
//   third_party/webrtc/**          sources, headers, licenses
//   third_party/sonic/**, third_party/minimp3/**
//   cmake/webrtc_sources.cmake     source lists per group
//   third_party/VENDOR.json        pins and file counts
//   LICENSE                       Flutter license registry notices
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'licenses.dart' as licenses;

final _includeRe = RegExp(
  r'^\s*#\s*(?:include|import)\s*"([^"]+)"',
  multiLine: true,
);

Future<void> main(List<String> args) async {
  final root = File.fromUri(Platform.script).parent.parent.path;
  final home = Platform.environment['HOME'] ?? '';
  var webrtcSrc = '$home/dev/libwebrtc/build/src';
  String? genDir;
  for (var i = 0; i < args.length; i++) {
    switch (args[i]) {
      case '--webrtc-src':
        webrtcSrc = args[++i];
      case '--gen-dir':
        genDir = args[++i];
      default:
        stderr.writeln('Unknown argument ${args[i]}');
        exit(64);
    }
  }
  genDir ??= '$webrtcSrc/../out-pcm-macos/gen';
  final manifest =
      jsonDecode(File('$root/tool/vendor_manifest.json').readAsStringSync())
          as Map<String, dynamic>;
  final webrtc = manifest['webrtc'] as Map<String, dynamic>;

  final main = await _GitTree.open(webrtcSrc, webrtc['commit'] as String);
  final thirdParty = await _GitTree.open(
    '$webrtcSrc/third_party',
    webrtc['third_party_commit'] as String,
  );
  final tree = _WebRtcTree(
    main,
    thirdParty,
    excludes: (webrtc['exclude_prefixes'] as List).cast<String>(),
    stubs: (webrtc['stubs'] as List).cast<String>().toSet(),
  );

  final groups = (webrtc['sources'] as Map<String, dynamic>).map(
    (k, v) => MapEntry(k, (v as List).cast<String>()),
  );
  final roots = (webrtc['include_roots'] as List).cast<String>();
  final generated = (webrtc['generated'] as List).cast<String>();

  // Include closure.
  final files = <String>{};
  final unresolved = <String>{};
  final queue = <String>[for (final g in groups.values) ...g];
  for (final s in queue) {
    if (!tree.exists(s)) throw StateError('Listed source is missing: $s');
  }
  while (queue.isNotEmpty) {
    final path = queue.removeLast();
    if (!files.add(path)) continue;
    final text = await tree.read(path);
    for (final m in _includeRe.allMatches(text)) {
      final inc = m.group(1)!;
      if (generated.contains(inc) || tree.stubs.contains(inc)) continue;
      final dir = path.contains('/')
          ? path.substring(0, path.lastIndexOf('/'))
          : '';
      final candidates = [
        for (final r in roots) r.isEmpty ? inc : '$r/$inc',
        if (dir.isNotEmpty) _normalize('$dir/$inc') else inc,
      ];
      final hit = candidates.where(tree.exists).firstOrNull;
      if (hit == null) {
        unresolved.add(inc);
      } else if (!files.contains(hit)) {
        queue.add(hit);
      }
    }
  }

  // License files in any ancestor folder of a vendored file.
  final licenseNames = {
    'LICENSE',
    'LICENSE.txt',
    'LICENSE.md',
    'COPYING',
    'PATENTS',
    'AUTHORS',
    'README.chromium',
    'NOTICE',
  };
  final dirs = <String>{};
  for (final f in files) {
    var d = f;
    while (d.contains('/')) {
      d = d.substring(0, d.lastIndexOf('/'));
      dirs.add(d);
    }
  }
  dirs.add('');
  for (final d in dirs) {
    for (final n in licenseNames) {
      final p = d.isEmpty ? n : '$d/$n';
      if (tree.exists(p)) files.add(p);
    }
  }

  final outDir = Directory('$root/third_party/webrtc');
  if (outDir.existsSync()) outDir.deleteSync(recursive: true);
  for (final f in files.toList()..sort()) {
    final out = File('${outDir.path}/$f');
    out.parent.createSync(recursive: true);
    out.writeAsBytesSync(await tree.readBytes(f));
  }
  // absl in its own inline namespace (ADR D5).
  final options = File(
    '${outDir.path}/third_party/abseil-cpp/absl/base/options.h',
  );
  var opt = options.readAsStringSync();
  opt = _replaceOnce(
    opt,
    '#define ABSL_OPTION_USE_INLINE_NAMESPACE 0',
    '#define ABSL_OPTION_USE_INLINE_NAMESPACE 1',
  );
  opt = _replaceOnce(
    opt,
    '#define ABSL_OPTION_INLINE_NAMESPACE_NAME head',
    '#define ABSL_OPTION_INLINE_NAMESPACE_NAME tsnx_absl',
  );
  options.writeAsStringSync(opt);
  for (final s in tree.stubs) {
    File('${outDir.path}/$s')
      ..parent.createSync(recursive: true)
      ..writeAsStringSync(
        '// Empty stand-in written by tool/vendor.dart. telosnex_audio builds\n'
        '// without Perfetto; RTC_USE_PERFETTO is not defined, so the trace\n'
        '// macros in rtc_base/trace_event.h are no-ops.\n',
      );
  }
  for (final g in generated) {
    final src = File('$genDir/$g');
    if (!src.existsSync()) {
      throw StateError('Generated header missing: ${src.path}');
    }
    src.copySync(
      (File('${outDir.path}/gen/$g')..parent.createSync(recursive: true)).path,
    );
  }

  // CMake source lists.
  final cmake = StringBuffer()
    ..writeln('# Generated by tool/vendor.dart. Do not edit.')
    ..writeln('# WebRTC ${webrtc['commit']}');
  for (final e in groups.entries) {
    cmake.writeln('set(TSNX_WEBRTC_SOURCES_${e.key.toUpperCase()}');
    for (final s in e.value) {
      cmake.writeln('  \${TSNX_WEBRTC_DIR}/$s');
    }
    cmake.writeln(')');
  }
  File('$root/cmake/webrtc_sources.cmake')
    ..parent.createSync(recursive: true)
    ..writeAsStringSync(cmake.toString());

  await main.close();
  await thirdParty.close();

  // Small single-repo dependencies.
  final pins = <String, Object>{
    'webrtc': {
      'url': webrtc['url'],
      'commit': webrtc['commit'],
      'third_party_commit': webrtc['third_party_commit'],
      'files': files.length + tree.stubs.length + generated.length,
    },
  };
  for (final name in ['sonic', 'minimp3']) {
    final dep = manifest[name] as Map<String, dynamic>;
    await _vendorSmall(root, name, dep);
    pins[name] = {'url': dep['url'], 'commit': dep['commit']};
  }
  File(
    '$root/third_party/VENDOR.json',
  ).writeAsStringSync('${const JsonEncoder.withIndent('  ').convert(pins)}\n');
  File(
    '$root/LICENSE',
  ).writeAsStringSync(licenses.bundledLicenses(Directory(root)));

  stdout.writeln(
    'webrtc: ${files.length} files, ${unresolved.length} unresolved includes (system or excluded).',
  );
  stdout.writeln(
    'This run replaced third_party/webrtc with upstream files. Local WebRTC '
    'commits (ALSA, MSVC fixes) are now reverted in the working tree; restore '
    'them with `git checkout HEAD -- <files>` or cherry-pick them. List: '
    '`git log --oneline -- third_party/webrtc`.',
  );
  final suspicious =
      unresolved
          .where(
            (u) =>
                u.contains('/') &&
                !u.startsWith('third_party/perfetto') &&
                !u.startsWith('third_party/protobuf'),
          )
          .toList()
        ..sort();
  if (suspicious.isNotEmpty) {
    stdout.writeln(
      'Unresolved project-style includes (check these are behind #if):',
    );
    for (final u in suspicious) {
      stdout.writeln('  $u');
    }
  }
}

String _replaceOnce(String s, String from, String to) {
  if (!s.contains(from)) throw StateError('Patch anchor not found: $from');
  return s.replaceFirst(from, to);
}

String _normalize(String p) {
  final out = <String>[];
  for (final part in p.split('/')) {
    if (part == '..') {
      if (out.isNotEmpty) out.removeLast();
    } else if (part != '.' && part.isNotEmpty) {
      out.add(part);
    }
  }
  return out.join('/');
}

Future<void> _vendorSmall(
  String root,
  String name,
  Map<String, dynamic> dep,
) async {
  final tmp = Directory.systemTemp.createTempSync('tsnx_vendor_$name');
  try {
    await _run('git', ['init', '-q'], tmp.path);
    await _run('git', [
      'fetch',
      '-q',
      '--depth',
      '1',
      dep['url'] as String,
      dep['commit'] as String,
    ], tmp.path);
    await _run('git', ['checkout', '-q', 'FETCH_HEAD'], tmp.path);
    final out = Directory('$root/third_party/$name');
    if (out.existsSync()) out.deleteSync(recursive: true);
    out.createSync(recursive: true);
    for (final f in (dep['files'] as List).cast<String>()) {
      File('${tmp.path}/$f').copySync('${out.path}/$f');
    }
  } finally {
    tmp.deleteSync(recursive: true);
  }
}

Future<String> _run(String exe, List<String> args, String cwd) async {
  final r = await Process.run(exe, args, workingDirectory: cwd);
  if (r.exitCode != 0) {
    throw ProcessException(exe, args, '${r.stderr}', r.exitCode);
  }
  return r.stdout as String;
}

/// Maps WebRTC paths to the main repository or the Chromium third_party one.
class _WebRtcTree {
  _WebRtcTree(
    this.main,
    this.thirdParty, {
    required this.excludes,
    required this.stubs,
  });
  final _GitTree main;
  final _GitTree thirdParty;
  final List<String> excludes;
  final Set<String> stubs;

  bool _excluded(String p) => excludes.any(p.startsWith);

  (_GitTree, String) _locate(String p) => p.startsWith('third_party/')
      ? (thirdParty, p.substring('third_party/'.length))
      : (main, p);

  bool exists(String p) {
    if (_excluded(p)) return false;
    final (t, rel) = _locate(p);
    return t.paths.contains(rel);
  }

  Future<String> read(String p) async =>
      utf8.decode(await readBytes(p), allowMalformed: true);

  Future<List<int>> readBytes(String p) {
    final (t, rel) = _locate(p);
    return t.blob(rel);
  }
}

/// Reads files of one commit through `git cat-file --batch`.
class _GitTree {
  _GitTree._(this.paths, this._proc);
  final Set<String> paths;
  final Process _proc;
  final _pending = <Completer<List<int>>>[];
  final _buf = <int>[];
  final _cache = <String, List<int>>{};
  Future<void> _lock = Future.value();

  static Future<_GitTree> open(String dir, String commit) async {
    final ls = await _run('git', ['ls-tree', '-r', '--name-only', commit], dir);
    final proc = await Process.start('git', [
      'cat-file',
      '--batch',
    ], workingDirectory: dir);
    final t = _GitTree._(
      ls.split('\n').where((l) => l.isNotEmpty).toSet(),
      proc,
    );
    t._commit = commit;
    proc.stdout.listen(t._onData);
    proc.stderr.listen(stderr.add);
    return t;
  }

  late final String _commit;

  void _onData(List<int> data) {
    _buf.addAll(data);
    while (_pending.isNotEmpty) {
      final nl = _buf.indexOf(10);
      if (nl < 0) return;
      final header = ascii.decode(_buf.sublist(0, nl));
      final parts = header.split(' ');
      if (parts.length < 3) {
        _buf.removeRange(0, nl + 1);
        _pending.removeAt(0).completeError(StateError('git cat-file: $header'));
        continue;
      }
      final size = int.parse(parts[2]);
      if (_buf.length < nl + 1 + size + 1) return;
      final body = _buf.sublist(nl + 1, nl + 1 + size);
      _buf.removeRange(0, nl + 1 + size + 1);
      _pending.removeAt(0).complete(body);
    }
  }

  Future<List<int>> blob(String rel) {
    final cached = _cache[rel];
    if (cached != null) return Future.value(cached);
    final prev = _lock;
    final done = Completer<List<int>>();
    _lock = done.future.then((_) {}, onError: (_) {});
    return prev.then((_) {
      final c = Completer<List<int>>();
      _pending.add(c);
      _proc.stdin.writeln('$_commit:$rel');
      return c.future.then(
        (b) {
          _cache[rel] = b;
          done.complete(b);
          return b;
        },
        onError: (Object e) {
          done.completeError(e);
          throw e;
        },
      );
    });
  }

  Future<void> close() async {
    await _proc.stdin.close();
    await _proc.exitCode;
  }
}
