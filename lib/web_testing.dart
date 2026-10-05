/// Test probes for the web engine. Not for app code.
library;

export 'src/web_testing_stub.dart'
    if (dart.library.js_interop) 'src/engine_web.dart'
    show
        WebOutputTap,
        debugContextState,
        debugIdle,
        debugResume,
        debugSetOutputDelay,
        debugTapOutput;
