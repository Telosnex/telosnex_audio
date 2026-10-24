# Native libraries and web runtime

The build hook gives each app one library, `telosnex_audio`, for the target
of the build. The library exports only the `tsnx_*` functions. For a web
build, the hook installs the web runtime files instead.

`prebuilt.json` pins the library of each target by SHA-256. It names the
GitHub release that holds the library. `web.json` pins the web runtime files
of the same release. The workflow `.github/workflows/kiln_release.yml`
writes both files (kiln ADR 001 and ADR 002).

The hook uses the release only when the package sources match its source
key. Run `dart run kiln:key` to show the key. Run
`dart run kiln:check` to check both manifests.

## Targets

| Target | Minimum | Release build |
|---|---|---|
| Android arm, arm64, x64 | API 24 | ubuntu-22.04, NDK 28.2.13676358 |
| iOS arm64 device, arm64 and x64 simulators | iOS 13 | macos-15, Xcode |
| macOS arm64, x64 | macOS 12 | macos-15, Xcode |
| Linux arm64, x64 | glibc 2.35 | ubuntu-22.04, GCC 11 |
| Windows x64, arm64 | Windows 10 | MSVC, Visual Studio 18 |
| Web | AudioWorklet and WebAssembly | ubuntu-24.04, `emscripten/emsdk:5.0.0` by digest |

`prebuilt.json` and `web.json` have the exact runner and toolchain of each
target.

iOS uses iOS 13 because Flutter gives build hooks iOS 13 for every app. The
64-bit Android libraries use 16 KB pages. The release workflow checks the
glibc version and the page alignment.

## Web runtime files

The web runtime has three files:

- `telosnex_audio.wasm`: the engine core. The release workflow builds it
  with `tool/kiln/build_web.dart`.
- `lib/web/telosnex_audio_worklet.js`: the AudioWorklet processor.
- `lib/web/telosnex_audio_storage.js`: the Worker that keeps the backing
  store.

In a Flutter web build, the hook installs the three files in
`web/kiln/telosnex_audio/` of the app. The engine loads each
file with the query `v=<webRuntimeVersion>`. The constant is in
`lib/src/web_runtime.g.dart`. Thus a browser does not use a cached file of
another release.

## Procedures

To build a library from source, set the user define `native_build: source`
for `telosnex_audio` in the pubspec of the app. The build needs CMake 3.22
or newer and the compiler of the target. Android needs the NDK.

To change `telosnex_audio_worklet.js` or `telosnex_audio_storage.js`:

1. Edit the file.
2. Run `dart run kiln:web stamp`.
3. Commit the file, `web.json` and `lib/src/web_runtime.g.dart`.

To run the web tests on the released `telosnex_audio.wasm`:

```bash
dart tool/kiln/test_web.dart
```

To run the web tests on a local build, activate Emscripten 5.0.0 or put it
at `../emsdk`. Then run:

```bash
dart tool/kiln/build_web.dart --out /tmp/telosnex_audio_web
dart tool/kiln/test_web.dart --from /tmp/telosnex_audio_web
```

The tests need Chrome. `web_test.dart` runs `kiln:web
install-dev`, which puts `telosnex_audio.wasm` in `lib/web/`. Git ignores
that file.

To make a release:

1. Merge the change to `main`.
2. Run `git push -f origin HEAD:kiln-release` from `main`.
3. When the workflow is done, open a pull request from the branch
   `kiln-manifest/<tag>`. The workflow cannot open it.
4. Merge the pull request.

Run `git push -f origin HEAD:kiln-release-dry` to build all files with no
upload.
