// Tells you when the native code that telosnex_audio vendors has an update.
//
// Usage:
//   dart .github/vendor_watch.dart
//
// It reads the pins in tool/vendor_manifest.json and asks GitHub (through
// the `gh` command) about each upstream. It finds:
//
//   - commits on the pinned WebRTC branch (webrtc.branch) after the pin
//     that change vendored files (commits that only add files next to
//     them are listed, but are not an update);
//   - a webrtc-sdk milestone branch (mNNN_release) newer than both the
//     pinned branch and webrtc.watch.seen_milestone;
//   - commits on the default branch of Sonic or minimp3 after the pin that
//     change their vendored files;
//   - a Chromium third_party pin that is not the one in DEPS at the
//     pinned WebRTC commit.
//
// It ignores commits listed in "watch": {"ignore": [...]} of a source.
// It exits 1 if it finds one of these, so the scheduled workflow
// (.github/workflows/vendor_watch.yml) fails and GitHub sends an email.
// VENDORING.md tells you what to do about each one.
import 'dart:convert';
import 'dart:io';

Future<void> main(List<String> args) async {
  final root = File.fromUri(Platform.script).parent.parent.path;
  final manifest =
      jsonDecode(File('$root/tool/vendor_manifest.json').readAsStringSync())
          as Map<String, dynamic>;
  final alerts = <Alert>[];
  final notes = <String>[];

  final webrtc = manifest['webrtc'] as Map<String, dynamic>;
  final webrtcRepo = gitHubRepo(webrtc['url'] as String);
  final pin = webrtc['commit'] as String;
  final branch = webrtc['branch'] as String;
  final watch = (webrtc['watch'] as Map<String, dynamic>?) ?? const {};
  final ignore = {...(watch['ignore'] as List<dynamic>? ?? const [])};

  // Commits on the pinned branch after the pin.
  final compare = await _api('repos/$webrtcRepo/compare/$pin...$branch');
  final status = compare['status'] as String;
  if (status == 'diverged' || status == 'behind') {
    alerts.add(
      Alert(
        'WebRTC pin is not on $branch',
        'The pin ${_short(pin)} is not an ancestor of $branch '
            '(compare status: $status). Find the commit on the branch that '
            'replaces it, or pin the branch head.',
      ),
    );
  } else {
    final commits = (compare['commits'] as List<dynamic>)
        .cast<Map<String, dynamic>>();
    final relevant = <String>[];
    final addsOnly = <String>[];
    for (final c in commits) {
      final sha = c['sha'] as String;
      if (ignore.contains(sha)) continue;
      final detail = await _api('repos/$webrtcRepo/commits/$sha');
      final files = [
        for (final f in detail['files'] as List<dynamic>)
          (
            (f as Map<String, dynamic>)['filename'] as String,
            added: f['status'] == 'added',
          ),
      ];
      final hits = vendoredHits(files, '$root/third_party/webrtc');
      final text = '${_line(c)}\n${hits.map((h) => '    $h').join('\n')}';
      if (hits.any((h) => !h.endsWith(_newSuffix))) {
        relevant.add(text);
      } else if (hits.isNotEmpty) {
        addsOnly.add(text);
      }
    }
    if (addsOnly.isNotEmpty) {
      notes.add(
        'WebRTC $branch: ${addsOnly.length} commit(s) only add files to '
        'vendored directories. They matter only if a vendored source starts '
        'to include them:\n${addsOnly.join('\n')}',
      );
    }
    if (relevant.isNotEmpty) {
      alerts.add(
        Alert(
          'WebRTC $branch has ${relevant.length} new commit(s) that change '
              'vendored files',
          'Pin: ${_short(pin)}. Vendor the head of $branch '
              '(VENDORING.md, "Update on the same branch").\n'
              '${relevant.join('\n')}',
        ),
      );
    }
    notes.add(
      'WebRTC $branch: ${commits.length} commit(s) after the pin, '
      '${relevant.length} that change vendored files.',
    );
  }

  // A newer milestone branch.
  final branches = await _apiList('repos/$webrtcRepo/branches');
  final pinned = milestoneOf(branch);
  final seen = watch['seen_milestone'] as int? ?? pinned ?? 0;
  final newest = newestMilestone([
    for (final b in branches) (b as Map<String, dynamic>)['name'] as String,
  ]);
  if (newest != null) {
    final newestBranch = 'm${newest}_release';
    notes.add(
      'WebRTC milestones: pinned $branch, newest $newestBranch, '
      'seen_milestone $seen.',
    );
    if (newest > seen && newest > (pinned ?? 0)) {
      final head = await _api('repos/$webrtcRepo/commits/$newestBranch');
      alerts.add(
        Alert(
          'webrtc-sdk has a new milestone branch: $newestBranch',
          'Pinned: $branch. Newest: $newestBranch, head ${_line(head)}.\n'
              'Move to it (VENDORING.md, "Move to a new milestone"), or '
              'set webrtc.watch.seen_milestone to $newest in '
              'tool/vendor_manifest.json to stop this alert until the next '
              'milestone.',
        ),
      );
    }
  }

  // The Chromium third_party pin must be the one that WebRTC's DEPS names.
  final deps = await _file(webrtcRepo, 'DEPS', pin);
  final depsThirdParty = thirdPartyCommitInDeps(deps);
  final thirdPartyPin = webrtc['third_party_commit'] as String;
  if (depsThirdParty != thirdPartyPin) {
    alerts.add(
      Alert(
        'Chromium third_party pin does not match WebRTC DEPS',
        'tool/vendor_manifest.json: ${_short(thirdPartyPin)}. DEPS at '
            '${_short(pin)}: ${depsThirdParty == null ? 'not found' : _short(depsThirdParty)}.',
      ),
    );
  }

  // Sonic and minimp3 follow their default branch.
  for (final name in ['sonic', 'minimp3']) {
    final source = manifest[name] as Map<String, dynamic>;
    final repo = gitHubRepo(source['url'] as String);
    final sourcePin = source['commit'] as String;
    final sourceIgnore = {
      ...((source['watch'] as Map<String, dynamic>?)?['ignore']
              as List<dynamic>? ??
          const []),
    };
    final files = {...(source['files'] as List<dynamic>).cast<String>()};
    final info = await _api('repos/$repo');
    final defaultBranch = info['default_branch'] as String;
    final cmp = await _api('repos/$repo/compare/$sourcePin...$defaultBranch');
    final relevant = <String>[];
    for (final c
        in (cmp['commits'] as List<dynamic>).cast<Map<String, dynamic>>()) {
      final sha = c['sha'] as String;
      if (sourceIgnore.contains(sha)) continue;
      final detail = await _api('repos/$repo/commits/$sha');
      final hits = [
        for (final f in detail['files'] as List<dynamic>)
          if (files.contains((f as Map<String, dynamic>)['filename']))
            f['filename'] as String,
      ];
      if (hits.isNotEmpty) relevant.add('${_line(c)} (${hits.join(', ')})');
    }
    notes.add(
      '$name $defaultBranch: ${cmp['ahead_by']} commit(s) after the pin, '
      '${relevant.length} that change vendored files.',
    );
    if (relevant.isNotEmpty) {
      alerts.add(
        Alert(
          '$name has ${relevant.length} new commit(s) that change vendored '
              'files',
          'Pin: ${_short(sourcePin)}. Update it (VENDORING.md, "Update '
              'Sonic or minimp3").\n${relevant.join('\n')}',
        ),
      );
    }
  }

  _report(alerts, notes);
  exitCode = alerts.isEmpty ? 0 : 1;
}

/// A finding that needs action.
class Alert {
  const Alert(this.title, this.detail);
  final String title;
  final String detail;
}

/// `owner/repo` from a GitHub clone URL.
String gitHubRepo(String url) {
  final m = RegExp(
    r'github\.com[/:]([^/]+/[^/]+?)(?:\.git)?/?$',
  ).firstMatch(url);
  if (m == null) throw FormatException('Not a GitHub URL', url);
  return m[1]!;
}

/// The milestone of a webrtc-sdk release branch: 144 for `m144_release`.
int? milestoneOf(String branch) {
  final m = RegExp(r'^m(\d+)_release$').firstMatch(branch);
  return m == null ? null : int.parse(m[1]!);
}

/// The highest milestone among [branches], or null.
int? newestMilestone(Iterable<String> branches) {
  int? newest;
  for (final b in branches) {
    final m = milestoneOf(b);
    if (m != null && (newest == null || m > newest)) newest = m;
  }
  return newest;
}

/// The Chromium `src/third_party` commit that a WebRTC DEPS file names.
String? thirdPartyCommitInDeps(String deps) => RegExp(
  r"""'src/third_party':\s*'[^'@]*/chromium/src/third_party@([0-9a-f]{40})'""",
).firstMatch(deps)?[1];

/// The files of a commit (paths relative to the WebRTC repository) that are
/// vendored under [vendorDir], and the files it adds to a vendored directory.
/// A new file there can be one that the vendored sources then include.
List<String> vendoredHits(
  Iterable<(String, {bool added})> files,
  String vendorDir, {
  bool Function(String path)? fileExists,
  bool Function(String path)? dirExists,
}) {
  final isFile = fileExists ?? (p) => File(p).existsSync();
  final isDir = dirExists ?? (p) => Directory(p).existsSync();
  return [
    for (final (path, :added) in files)
      if (isFile('$vendorDir/$path'))
        path
      else if (added &&
          path.contains('/') &&
          isDir('$vendorDir/${path.substring(0, path.lastIndexOf('/'))}'))
        '$path$_newSuffix',
  ];
}

const _newSuffix = ' (new, in a vendored directory)';

String _short(String sha) => sha.substring(0, 10);

String _line(Map<String, dynamic> commit) {
  final c = commit['commit'] as Map<String, dynamic>;
  final date = ((c['committer'] as Map<String, dynamic>)['date'] as String)
      .substring(0, 10);
  final subject = (c['message'] as String).split('\n').first;
  return '${_short(commit['sha'] as String)} $date $subject';
}

Future<Map<String, dynamic>> _api(String path) async =>
    jsonDecode(await _gh(['api', path])) as Map<String, dynamic>;

Future<List<dynamic>> _apiList(String path) async {
  // --slurp joins the pages into one array of pages.
  final pages =
      jsonDecode(await _gh(['api', '--paginate', '--slurp', path]))
          as List<dynamic>;
  return [for (final page in pages) ...page as List<dynamic>];
}

Future<String> _file(String repo, String path, String ref) => _gh([
  'api',
  '-H',
  'Accept: application/vnd.github.raw',
  'repos/$repo/contents/$path?ref=$ref',
]);

Future<String> _gh(List<String> args) async {
  final r = await Process.run('gh', args);
  if (r.exitCode != 0) {
    stderr.writeln('gh ${args.join(' ')} failed:\n${r.stderr}');
    exit(2);
  }
  return r.stdout as String;
}

void _report(List<Alert> alerts, List<String> notes) {
  final ci = Platform.environment['GITHUB_ACTIONS'] == 'true';
  final summary = StringBuffer(
    alerts.isEmpty
        ? '## Vendored native code: up to date\n\n'
        : '## Vendored native code: ${alerts.length} update(s)\n\n',
  );
  for (final a in alerts) {
    stdout
      ..writeln('UPDATE: ${a.title}')
      ..writeln(a.detail.split('\n').map((l) => '  $l').join('\n'))
      ..writeln();
    if (ci) stdout.writeln('::error title=${a.title}::See the job summary.');
    summary
      ..writeln('### ${a.title}\n')
      ..writeln('```\n${a.detail}\n```\n');
  }
  for (final n in notes) {
    stdout.writeln(n);
    summary.writeln('- $n');
  }
  final path = Platform.environment['GITHUB_STEP_SUMMARY'];
  if (path != null && path.isNotEmpty) {
    File(path).writeAsStringSync('$summary', mode: FileMode.append);
  }
}
