import 'dart:io';

import 'package:test/test.dart';

import '../tool/licenses.dart';

void main() {
  final root = Directory.current;
  test('Flutter license file includes every vendored license verbatim', () {
    final bundle = bundledLicenses(root);
    expect(File('${root.path}/LICENSE').readAsStringSync(), bundle);
    final sections = bundle.split('\n${'-' * 80}\n');
    expect(sections, hasLength(licenseSources.length));
    for (final entry in licenseSources.entries) {
      final section = sections.singleWhere(
        (s) => s.startsWith('telosnex_audio\n${entry.value}\n\n'),
      );
      expect(section, contains(File(entry.key).readAsStringSync()));
      expect(section.indexOf('\n\n'), greaterThan(0));
    }
    expect(bundle, contains('Copyright 2010 Bill Cox'));
    expect(bundle, contains('Additional IP Rights Grant (Patents)'));
  });

  test('new vendored notices cannot be omitted', () {
    final temp = Directory.systemTemp.createTempSync('tsnx_licenses_test_');
    addTearDown(() => temp.deleteSync(recursive: true));
    File('${temp.path}/third_party/new_dependency/LICENSE')
      ..parent.createSync(recursive: true)
      ..writeAsStringSync('upstream license');
    expect(() => bundledLicenses(temp), throwsStateError);
  });
}
