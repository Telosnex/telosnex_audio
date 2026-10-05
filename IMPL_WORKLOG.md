# telosnex_audio implementation worklog

ADR: ~/dev/telosnex/docs/design/ADR_telosnex_audio.md (ACCEPTED 2026-10-04).
Scope: workplan steps 1-6 (done), then 7 onward.

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
