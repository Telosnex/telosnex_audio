// Runs example/lib/web_check.dart in a browser and prints its report.
//
//   cd example && flutter build web -t lib/web_check.dart && cd ..
//   dart tool/web_check.dart --browser chrome         headless Chrome
//   dart tool/web_check.dart --browser chrome-window  Chrome on the speakers
//   dart tool/web_check.dart --browser firefox        headless Firefox
//   dart tool/web_check.dart --browser safari         Safari via safaridriver
//   dart tool/web_check.dart --browser none           prints the URL
//
// Serves example/build/web on localhost, opens the page with
// ?report=<this server>/report, and exits with the page's result.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

/// Chrome: CHROME_EXECUTABLE (as for Flutter), else the macOS app.
String get _chrome =>
    Platform.environment['CHROME_EXECUTABLE'] ??
    '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
String get _firefox =>
    Platform.environment['FIREFOX_EXECUTABLE'] ??
    '/Applications/Firefox.app/Contents/MacOS/firefox';
const _types = {
  '.html': 'text/html',
  '.js': 'text/javascript',
  '.mjs': 'text/javascript',
  '.wasm': 'application/wasm',
  '.json': 'application/json',
  '.png': 'image/png',
  '.ttf': 'font/ttf',
  '.otf': 'font/otf',
  '.mp3': 'audio/mpeg',
};

Future<void> main(List<String> args) async {
  final browser = args.contains('--browser')
      ? args[args.indexOf('--browser') + 1]
      : 'chrome';
  final root = Directory.fromUri(
    Platform.script.resolve('../example/build/web/'),
  );
  if (!File('${root.path}/index.html').existsSync()) {
    stderr.writeln(
      'No web build. Run: cd example && flutter build web -t lib/web_check.dart',
    );
    exit(2);
  }
  final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
  final report = Completer<String>();
  // The page's log lines as they happen (shown when it never reports).
  final progress = <String>[];
  server.listen((req) async {
    if (req.method == 'POST' && req.uri.path == '/report/line') {
      final line = await utf8Body(req);
      req.response.statusCode = 204;
      await req.response.close();
      progress.add(line);
      return;
    }
    if (req.method == 'POST' && req.uri.path == '/report') {
      final body = await utf8Body(req);
      req.response.statusCode = 204;
      await req.response.close();
      if (!report.isCompleted) report.complete(body);
      return;
    }
    final path = req.uri.path == '/' ? '/index.html' : req.uri.path;
    final file = File('${root.path}${Uri.decodeComponent(path)}');
    if (!file.existsSync()) {
      req.response.statusCode = 404;
      await req.response.close();
      return;
    }
    final dot = path.lastIndexOf('.');
    final type = dot < 0 ? null : _types[path.substring(dot)];
    if (type != null) req.response.headers.set('content-type', type);
    await req.response.addStream(file.openRead());
    await req.response.close();
  });
  final base = 'http://localhost:${server.port}';
  final url = '$base/?report=${Uri.encodeComponent('$base/report')}';
  stdout.writeln('serving $url');

  Process? browserProcess;
  Directory? profile;
  _SafariSession? safari;
  if (browser == 'chrome' || browser == 'chrome-window') {
    profile = Directory.systemTemp.createTempSync('tsnx_chrome');
    browserProcess = await Process.start(_chrome, [
      if (browser == 'chrome') '--headless=new',
      '--autoplay-policy=no-user-gesture-required',
      // A fake microphone (a beep) without a permission prompt.
      '--use-fake-ui-for-media-stream',
      '--use-fake-device-for-media-stream',
      '--user-data-dir=${profile.path}',
      '--no-first-run',
      url,
    ]);
  } else if (browser == 'firefox') {
    profile = Directory.systemTemp.createTempSync('tsnx_firefox');
    File('${profile.path}/user.js').writeAsStringSync('''
user_pref("media.autoplay.default", 0);
user_pref("media.autoplay.blocking_policy", 0);
user_pref("media.navigator.permission.disabled", true);
user_pref("media.navigator.streams.fake", true);
''');
    browserProcess = await Process.start(_firefox, [
      '--headless',
      '--no-remote',
      '--profile',
      profile.path,
      url,
    ]);
  } else if (browser == 'safari') {
    safari = await _SafariSession.open(url);
  } else {
    stdout.writeln('Open $url in the browser.');
  }
  if (browserProcess != null) {
    unawaited(browserProcess.stdout.drain<void>());
    unawaited(browserProcess.stderr.drain<void>());
  }

  String result;
  try {
    result = await report.future.timeout(const Duration(minutes: 3));
  } on TimeoutException {
    result = [
      if (progress.isEmpty) 'the page logged nothing',
      ...progress,
      'FAIL timeout: no report in 3 min',
    ].join('\n');
  }
  browserProcess?.kill();
  await safari?.close();
  await server.close(force: true);
  try {
    profile?.deleteSync(recursive: true);
  } on FileSystemException {
    // Chrome can still hold files for a moment.
  }
  stdout.writeln(result);
  exit(result.trimRight().split('\n').last == 'PASS' ? 0 : 1);
}

Future<String> utf8Body(HttpRequest req) async {
  final bytes = <int>[];
  await for (final chunk in req) {
    bytes.addAll(chunk);
  }
  return String.fromCharCodes(bytes);
}

/// A Safari window driven by safaridriver (enable it once with
/// `sudo safaridriver --enable`). webkit:alwaysAllowAutoplay lets the
/// AudioContext run without a click, and automation sessions give
/// getUserMedia WebKit's mock microphones without a prompt.
class _SafariSession {
  _SafariSession(this._driver, this._base, this._id);

  final Process _driver;
  final Uri _base;
  final String _id;

  static Future<_SafariSession> open(String url) async {
    final socket = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
    final port = socket.port;
    await socket.close();
    final driver = await Process.start('safaridriver', ['--port', '$port']);
    unawaited(driver.stdout.drain<void>());
    unawaited(driver.stderr.drain<void>());
    final base = Uri.parse('http://localhost:$port/');
    final deadline = DateTime.now().add(const Duration(seconds: 20));
    while (true) {
      try {
        final status = await _request('GET', base.resolve('status'));
        if (status['ready'] == true) break;
      } on Object {
        // Not listening yet.
      }
      if (DateTime.now().isAfter(deadline)) {
        driver.kill();
        throw StateError(
          'safaridriver did not start; enable it once with '
          '`sudo safaridriver --enable`.',
        );
      }
      await Future<void>.delayed(const Duration(milliseconds: 200));
    }
    // Safari takes a few seconds to release the automation window of a
    // session that just ended ("already paired with another session").
    Map<String, Object?> session;
    for (var attempt = 1; ; attempt++) {
      try {
        session = await _request('POST', base.resolve('session'), {
          'capabilities': {
            'alwaysMatch': {
              'browserName': 'safari',
              'webkit:alwaysAllowAutoplay': true,
            },
          },
        });
        break;
      } on StateError catch (e) {
        if (attempt == 10) {
          driver.kill();
          rethrow;
        }
        stderr.writeln('$e; retrying');
        await Future<void>.delayed(const Duration(seconds: 3));
      }
    }
    final id = session['sessionId'] as String;
    stdout.writeln(
      'Safari ${(session['capabilities'] as Map)['browserVersion']}',
    );
    await _request('POST', base.resolve('session/$id/url'), {'url': url});
    return _SafariSession(driver, base, id);
  }

  Future<void> close() async {
    try {
      await _request('DELETE', _base.resolve('session/$_id'));
    } on Object catch (e) {
      stderr.writeln('closing the Safari session: $e');
    }
    _driver.kill();
  }

  static Future<Map<String, Object?>> _request(
    String method,
    Uri uri, [
    Object? body,
  ]) async {
    final client = HttpClient();
    try {
      final req = await client.openUrl(method, uri);
      if (body != null) {
        req.headers.contentType = ContentType.json;
        req.write(jsonEncode(body));
      }
      final res = await req.close();
      final text = await res.transform(utf8.decoder).join();
      final json = jsonDecode(text) as Map<String, Object?>;
      final value = json['value'];
      if (res.statusCode != 200) {
        throw StateError('safaridriver $method ${uri.path}: $value');
      }
      return value is Map<String, Object?> ? value : const {};
    } finally {
      client.close(force: true);
    }
  }
}
