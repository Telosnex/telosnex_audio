# telosnex_audio implementation worklog

ADR: ~/dev/telosnex/docs/design/ADR_telosnex_audio.md (ACCEPTED 2026-10-04).
Scope: package code through step 9. App migration (steps 5-6) is not done.
Hardware gates remain open as listed below.

## Facts
- WebRTC checkout: ~/dev/libwebrtc/build/src at webrtc-sdk b1800a61db (working tree has fork
  patches; vendor reads git objects, not the working tree). Chromium third_party a3b630c291f1.
- Spike list (443 TUs): /tmp/wrtc/srcs.txt. Spike gen headers: ~/dev/libwebrtc/build/out-pcm-macos/gen.
- Sonic b93885dcb70aae50c6f76b0fe4e0868f029a077e, minimp3 ea99364f61c14656440e8d77e9c233ccf3124633.
- Fork clock correction: ~/dev/libwebrtc branch telosnex/m144-pcm-playout, src/internal/{drift_servo,hardware_clock_estimator,capture_clock_policy,audio_clock_correction}.
- macOS fork ADM = AudioEngineDevice (AVAudioEngine, Apple voice processing).
- Shell for cli tool is sh: no process substitution.

## Status
- Step 1: freeze note added to telosnex lib/features/mic/docs/WEBRTC_MAINTENANCE.md (uncommitted).
- Step 2: vendored 1132 WebRTC files (10 MB) + Sonic + minimp3. CMake + hook (persistent build dir in
  outputDirectoryShared). Gate: A.2 probe builds from the vendored tree (arm64 + x86_64 under Rosetta);
  header edit -> example app running with the change in <=12 s; one WebRTC .cc touched -> app build 6 s.
- WARNING: ~/dev itself is a stray git repo (no commits, since 2026-03-16). Always check
  `git rev-parse --show-toplevel` before committing. On 2026-10-04 I committed into it by mistake and
  reverted: deleted the branch ref, logs, COMMIT_EDITMSG, the new 2.2 GB pack, the 9381 loose objects born
  after 14:50, and the index (which now referenced deleted objects). The earlier index content is lost.
  201 older loose objects got a new mtime.
- Step 3: macOS engine on the CoreAudio ADM + AEC3 (default) or AVAudioEngine + Apple VP (option).
  Real-speaker echo test (native_test/echo_probe.cc, MacBook Pro speakers at 44% volume, built-in mic,
  speech fixture at gain 0.5, NS on): residual after APM mean ~19 dB vs Apple VP path (the fork's macOS
  device) ~22 dB; mic level 56 dB. I2 test passes (format changes = 1; suppression 47.7/53.0/47.2 dB).
  Clock correction ported (src/clock) with fork tests; engine test engages at -2197 ppm for a 2000 ppm drift.
- Step 4: I1 (0 allocations after warm-up), I4 (p95 12.0 ms), I5, I6, I7, I11 tests pass. TSAN clean
  incl. a concurrent render/control test. I3 holds by construction (ProcessReverseStream on the device frame).
- Step 7 (code done; Pi echo acceptance owed): Linux (ALSA + PulseAudio by dlopen, headers from
  libasound2-dev/libpulse-dev) and Windows (Core Audio, MSVC). Fork patches are normal commits on the
  vendored tree: hardware clock API, ALSA capture depth, ALSA hw clock. MSVC needed two WebRTC fixes
  (denormal disabler asm guard, AVX2 matched filter subscripts) and /permissive on audio_device_core_win.cc.
  Native tests pass: Linux arm64 (Parallels), Linux x64 (orb tsnx-build), Debian bookworm arm64
  (docker), Windows arm64 and x64 (Parallels). Windows echo_probe on the VM device: opens, plays,
  captures. Linux ALSA echo_probe on the VM: opens, 10 ms period. Example app builds on Windows and
  Linux through the hook; the Windows app runs and lists devices (OCR of a VM screenshot).
- WARNING: re-running tool/vendor.dart wipes the local WebRTC commits; cherry-pick them again
  (`git log --oneline -- third_party/webrtc`).
- VM notes: prlctl exec joins its arguments into one shell string, so pass one quoted string.
  Ubuntu VM has no logged-in user: use `su - parallels -c '...'`. Windows VM gets sources by HTTP
  from the Ubuntu VM (10.211.55.4:8765); the Mac's 10.211.55.2 is not reachable from it.
  build/sync_win.sh pushes the tree. Wrap prlctl in `perl -e 'alarm N; exec @ARGV'`.
  /tmp/ocr (Swift Vision) reads text from `prlctl capture` screenshots.
- Step 8 iOS (code done, device run pending): builds for iphoneos and in the example app. The iPhone
  was locked, so lib/device_check.dart has not run. Run: flutter build ios --profile -t
  lib/device_check.dart; devicectl install build/ios/Profile-iphoneos/Runner.app; launch --console.
  I10 rewind + test (31 native tests). Android not started: needs owner decisions (see report).
- Step 8 Android (code done, emulator only): owner chose AEC3 (2026-10-05). src/android/aaudio_device.cc,
  USAGE_MEDIA always, mic VOICE_RECOGNITION, AAudio by dlopen. Emulator API 36 arm64: 31 native tests,
  echo_probe, device_check PASS. Open: armeabi-v7a (owner), real-phone echo (R13) and route matrix.
  Emulator must start detached: perl -MPOSIX -e 'fork and exit; POSIX::setsid(); exec @ARGV' emulator ...
  Native tests on device: cmake -DTSNX_FIXTURES_DIR=/data/local/tmp/fixtures; adb push binaries + fixtures.
- Owner 2026-10-05: Android needs mic and speaker selection at about parity with libwebrtc (flutter_webrtc).
  Owner chose (b): communication mode only while the earpiece or the Bluetooth mic is selected (ADR D6, D15).
  Done (emulator): src/android/android_routes.* (pure rules, native tests), android_jni.* (JNI_OnLoad via
  the plugin's System.loadLibrary on a background thread; exports.android adds JNI_OnLoad), android/ Java
  (AudioRoutes: getDevices, setMode + setCommunicationDevice / SCO before API 31). Emulator API 36: routes
  listed through JNI, device_check PASS in profile and release (R8 keep rules). The emulator has no earpiece:
  `adb shell setprop debug.tsnx.routes 1` adds output debug-speaker-call (speaker in communication mode);
  dumpsys showed MODE_IN_COMMUNICATION on select and MODE_NORMAL ~1 s after streams close.
  Open: real phone with a Bluetooth headset (ADR A.4 "Android routes").
- Owner 2026-10-05: current-route readback on every platform. Done: AudioEngine.currentOutput/currentInput
  (CurrentDevice: id = list entry in effect, name + AudioDeviceKind of the device in use), also on RouteChange;
  C tsnx_device_current. Engine resolves it in RefreshDevices; selects refresh before REQUEST_DONE. Platform
  change counters (Android JNI, iOS route notification, macOS CoreAudio listeners) trigger a refresh.
  Every list now starts with "default": Linux index 0 renamed from "0"; Windows gets a synthesized entry that
  maps to SetPlayoutDevice(kDefaultDevice) (before: index 0 = first endpoint, not the default). The default
  entry is named after its device ("default (X)" -> "X"). Kinds: Android (route rules), iOS (port type),
  macOS (transport + data source); other elsewhere.
  Checked: native tests 43 (Mac, emulator, Linux arm64, Windows arm64); `echo_probe --routes` PASS on macOS
  (CoreAudio + Apple VP ADMs), Linux Pulse and ALSA (root; user parallels is not in group audio), Windows;
  device_check PASS on macOS and the emulator; web_check PASS in Chrome (JS and wasm). iOS builds; the iPhone
  was locked, so the iOS readback has not run.

## Step 9 web (started 2026-10-05)
Design:
- One wasm module (src/web/web_core.cc + Track, TrackStore, Sonic, SincResampler, minimp3), standalone, no Emscripten JS.
  Built by tool/build_wasm.dart; committed under assets/web/ (hooks do not run on web).
- assets/web/telosnex_audio_worklet.js: TsnxCore (wasm wrapper, message protocol) + processors 'telosnex-audio'
  (engine: 1 mic input, stereo output, 480-frame FIFO over 128-frame quanta) and 'telosnex-tap' (test).
  The same file loads as a classic script on the main thread for ManualDevice (tests).
- assets/web/telosnex_audio_storage.js: worker = backing store (D11 web). .all PCM: Dart posts every write to it too;
  1 s chunks go to IndexedDB. MP3: worker opens/decodes with its own wasm instance. Worklet asks for missing
  window chunks over a MessageChannel; TrackStore "external" mode (request/deliver), testable natively.
- Clock: render clock = AudioContext frame time. delay = baseLatency + outputLatency. Dart nowNs() from
  getOutputTimestamp + latency. HeardPosition(nowNs) does not depend on the latency value.
- Capture (D15 web): getUserMedia (browser AEC) into the same node; wasm resampler to the asked rate.
Status (2026-10-05):
- Code done. 13 engine cases pass in Chrome (flutter test --platform chrome test/web_engine_test.dart) and
  12 on the VM. Real-time gate (example/lib/web_check.dart via tool/web_check.dart): headless Chrome 154 and
  Chrome on the speakers PASS, dart2js and dart2wasm. p95 ~10 ms, far seeks 51-59 ms.
- Open: Safari run needs a click (autoplay) -> `dart tool/web_check.dart --browser safari`.
  Firefox is not installed. App side (step 6 flag, then remove LocalPcmPlayout/just_audio) not started.
- flutter test web serves package:telosnex_audio/ at /packages/telosnex_audio/, not Flutter assets:
  engine_web falls back to that URL when the asset 404s. test/fixtures links to native_test/fixtures.

## Owner follow-up (2026-10-05)
- Created private GitHub repository Telosnex/telosnex_audio with origin. History scanned with
  gitleaks before the first push: no leaks in 30 commits. Repository visibility does not assign
  a license to Telosnex's own code.
- Risk 6: tool/licenses.dart builds LICENSE in Flutter's multi-component format. It includes all
  12 vendored license, COPYING, PATENTS, and NOTICE files, including WebRTC's embedded dependencies.
  tool/vendor.dart regenerates it. CI checks freshness. test/licenses_test.dart checks complete
  upstream text and rejects an unlisted license. A Flutter web build's NOTICES includes every
  upstream text verbatim. The app receives these notices when it adds the package dependency.
- Restored the fork freeze note in the app maintenance document. The ADR names implemented tests
  and records pending gates. The owner waived step 10 latency and web echo checks, not billing.
- Owner correction: the repository is public and uses standard GitHub-hosted runners, not WarpBuild.
  CI and release workflows now use macos-15 (arm64), ubuntu-24.04 (x64), ubuntu-24.04-arm, and
  windows-2025-vs2026. Checkout uses actions/checkout. actionlint passes. Removed the temporary
  repository access to the organization's self-hosted runner group. No WarpBuild app access is needed.
- armeabi-v7a now builds with NDK 28.2 at API 24. CMake selects ARM32 NEON and portable C SPL,
  without the ARMv7 assembly paths. The hook maps Architecture.arm to armeabi-v7a. The profile
  example APK contains lib/armeabi-v7a/libtelosnex_audio.so. Native tests compile for ARM32,
  including static lock-free checks for every atomic type used by the render path. They do not
  run on the current emulator, which advertises only arm64-v8a. Mac native tests: 44 passed.
- First GitHub-hosted CI: seven jobs passed. Web found a stale wasm input stamp from the prior
  device-readback commit (rebuilt; binary unchanged; Chrome cases pass). Windows native tests
  passed but the export regex matched the dumpbin characteristics header. The corrected regex
  accepts the real DLL and rejects an injected unexpected symbol in the Windows VM.
- Step 10 code is prepared in the app and Edge repositories. New billed WS endpoints cover
  standard Realtime Live, Transcribe, and Translate. The app defaults remain WebRTC. Standard
  Live also needs the step 6 Track-backed flush player before its factory can be enabled.
  Targeted app tests: 13 passed. The affected run had 806 passes and 27 unrelated shared-worktree
  failures. Edge local billing/lifecycle tests pass, with no provider or wallet calls.
  Real provider/billing parity and a universal terminal billing record on transport drops remain
  open. No endpoint deployment or transport-default change occurred. Latency/web echo checks
  were skipped as requested.
