import 'package:test/test.dart';

import '../.github/vendor_watch.dart';

void main() {
  test('milestones of release branches', () {
    expect(milestoneOf('m144_release'), 144);
    expect(milestoneOf('master'), isNull);
    expect(milestoneOf('m144_release_old'), isNull);
    expect(
      newestMilestone([
        'master',
        'm97_release',
        'm150_release',
        'm144_release',
      ]),
      150,
    );
    expect(newestMilestone(['master']), isNull);
  });

  test('GitHub repository of a clone URL', () {
    expect(
      gitHubRepo('https://github.com/webrtc-sdk/webrtc.git'),
      'webrtc-sdk/webrtc',
    );
    expect(gitHubRepo('https://github.com/lieff/minimp3'), 'lieff/minimp3');
    expect(
      () => gitHubRepo('https://chromium.googlesource.com/x'),
      throwsFormatException,
    );
  });

  test('Chromium third_party commit in DEPS', () {
    const sha = 'a3b630c291f1cf3b711687b1c4be322d3015512e';
    const deps =
        """
vars = {}
deps = {
  'src/third_party':
    'https://chromium.googlesource.com/chromium/src/third_party@$sha',

  'src/buildtools/linux64': {
""";
    expect(thirdPartyCommitInDeps(deps), sha);
    expect(thirdPartyCommitInDeps('deps = {}'), isNull);
  });

  test('vendored files, and files added to vendored directories', () {
    const vendored = {'/v/rtc_base/a.cc'};
    const dirs = {'/v/rtc_base'};
    final hits = vendoredHits(
      [
        ('rtc_base/a.cc', added: false),
        ('rtc_base/b.cc', added: false), // upstream file we do not vendor
        ('rtc_base/c.cc', added: true),
        ('sdk/d.mm', added: true), // directory not vendored
        ('BUILD.gn', added: true),
      ],
      '/v',
      fileExists: vendored.contains,
      dirExists: dirs.contains,
    );
    expect(hits, [
      'rtc_base/a.cc',
      'rtc_base/c.cc (new, in a vendored directory)',
    ]);
  });
}
