# telosnex_audio implementation worklog

ADR: ~/dev/telosnex/docs/design/ADR_telosnex_audio.md (ACCEPTED 2026-10-04).
Scope of this session: workplan steps 1-6.

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
