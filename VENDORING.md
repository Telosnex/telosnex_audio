# Vendored native code

telosnex_audio compiles C and C++ code from other projects. This file
tells you where that code comes from, how you know that it has an update,
and how you update it.

## Where the code comes from

| Source | Upstream | Pin | Vendored |
|---|---|---|---|
| WebRTC | [webrtc-sdk/webrtc](https://github.com/webrtc-sdk/webrtc), branch `m150_release` | `webrtc.commit` | Audio processing, audio devices, and the `rtc_base` and `api` files that they include. No network code. |
| Chromium `third_party` | `chromium/src/third_party` | `webrtc.third_party_commit` | Abseil, PFFFT, RNNoise, Perfetto headers |
| Sonic | [waywardgeek/sonic](https://github.com/waywardgeek/sonic), default branch | `sonic.commit` | `sonic.c`, `sonic.h` |
| minimp3 | [lieff/minimp3](https://github.com/lieff/minimp3), default branch | `minimp3.commit` | `minimp3.h`, `minimp3_ex.h` |

The pins are in `tool/vendor_manifest.json`. `tool/vendor.dart` copies the
pinned files into `third_party/`. `third_party/VENDOR.json` records what
the last copy used.

The WebRTC code passes through three projects:

1. Chromium makes a milestone about every four weeks (M144, M150).
2. Google WebRTC makes a branch for each milestone.
3. LiveKit keeps the `webrtc-sdk/webrtc` fork. Its `mNNN_release`
   branches add LiveKit fixes to a Google branch. Many of these fixes are
   for the Apple audio engine and for ALSA.

LiveKit does not make a branch for each Chromium milestone. Its
fixes go to its newest `mNNN_release` branch, and an older branch then
stops. The pinned branch must be the one that gets the fixes.

The Chromium `third_party` pin is not a choice. It must be the commit that
`DEPS` names at the pinned WebRTC commit.

## Local patches

Some vendored files have local changes. Each change is a separate commit
after the commit that vendored the files. A run of `tool/vendor.dart`
removes these changes from the working tree.

| Commit | Change |
|---|---|
| `e8b9876` | Hardware clock observations through `AudioTransport` |
| `f9e003a` | No GCC-style ARM assembly in the denormal disabler under MSVC |
| `b5e8e7c` | No vector subscripts in the AVX2 matched filter (MSVC) |
| `55b376a` | ALSA: 10 ms capture period with a 1 s ring |
| `817520b` | ALSA: hardware clock observations |
| `e7515ba` | macOS: start voice-processing capture once and sooner |
| `bb2901e` | `RtpPacketInfo` `operator==` defaulted in the class (MSVC) |

When you add a local patch, add it to this table. Merge a vendor pull
request with a merge commit, not a squash: a squash puts all patches into
one commit, and the next update cannot apply them one at a time.

## How you know about an update

The `Vendor watch` workflow (`.github/workflows/vendor_watch.yml`) runs
`.github/vendor_watch.dart` every day. The run fails when it finds one of
these updates:

| Finding | What to do |
|---|---|
| New commits on the pinned WebRTC branch that change vendored files | [Update on the same branch](#update-on-the-same-branch) |
| A `webrtc-sdk` milestone branch newer than `webrtc.watch.seen_milestone` | [Move to a new milestone](#move-to-a-new-milestone), or acknowledge it |
| New commits in Sonic or minimp3 that change vendored files | [Update Sonic or minimp3](#update-sonic-or-minimp3) |
| The Chromium `third_party` pin is not the one in `DEPS` | Set `webrtc.third_party_commit` to the commit in `DEPS`, then vendor again |

GitHub sends an email for a failed scheduled run to the person who last
changed the workflow file. The job summary lists each finding with its
commits.

To run the check on your computer, use `gh` with a login:

```sh
dart .github/vendor_watch.dart
```

### Acknowledge a finding

A new milestone is not always a reason to move. To stop the alert until
the next milestone, set `webrtc.watch.seen_milestone` to the new milestone
number.

A commit that does not apply to telosnex_audio can also stop the alert.
Add its full SHA to `watch.ignore` of its source, with the reason in the
commit message.

A push to `main` that changes `tool/vendor_manifest.json` runs the watch
again.

## Update on the same branch

Do this when the pinned WebRTC branch has new commits. Commit
`08d59ea` is an example.

1. Make two shallow clones in a temporary directory:

   ```sh
   git init src
   git -C src fetch --depth 1 https://github.com/webrtc-sdk/webrtc.git <new commit>
   git -C src update-ref refs/heads/pin FETCH_HEAD
   git init src/third_party
   git -C src/third_party remote add origin \
     https://chromium.googlesource.com/chromium/src/third_party
   git -C src/third_party fetch --depth 1 --filter=blob:none origin <third_party commit>
   ```

   Fetch WebRTC with its file contents (about 40 MB). With
   `--filter=blob:none`, git gets each file that the vendor tool reads
   separately, and the run takes many minutes instead of seconds.
   `third_party` is large, so fetch it without contents.

2. In `tool/vendor_manifest.json`, set `webrtc.commit` to the new commit.
   Get `webrtc.third_party_commit` from `DEPS` at the new commit.
3. Run the vendor tool:

   ```sh
   dart tool/vendor.dart --webrtc-src <temporary directory>/src
   ```

4. Commit the result. Put the upstream commits that change vendored files
   in the commit message. The watch output lists them.
5. Apply the [local patches](#local-patches) again with
   `git cherry-pick`. If upstream now includes a patch, drop it.
6. Update the commit SHAs in the local patches table.
7. Build and run the tests on every platform. Push to a `ci/` branch to
   run Integration.
8. Make a Kiln release. A change in `third_party/` changes the source key.

## Move to a new milestone

Do this when LiveKit starts a newer `mNNN_release` branch.

1. Read the LiveKit commits on the new branch. Find the commits that
   change vendored files.
2. Do the steps in [Update on the same branch](#update-on-the-same-branch)
   with the head of the new branch.
3. Set `webrtc.branch` to the new branch, and
   `webrtc.watch.seen_milestone` to its number.
4. If the vendor tool stops because a listed source is missing, find
   where the file went (`git -C src ls-tree -r --name-only HEAD`). Update
   or remove it in `tool/vendor_manifest.json`.
5. If the link fails with undefined symbols, upstream moved code into new
   files. Find them with `git grep` in the clone, and add them to the
   source lists.
6. Look for API changes in the compile errors of `src/`.

A new milestone can change many files. Expect conflicts in the local
patches, and compile errors on MSVC that macOS does not show. Run
Integration on a `ci/` or `vendor/` branch before you merge.

The move from M144 to M150 (pull request #29) is an example. Upstream
removed AECM (unused here), Abseil moved files, new files appeared in
`modules/audio_processing/capture_mixer/`, `webrtc::ArrayView` became
`std::span`, and MSVC needed one new local patch.

## Update Sonic or minimp3

1. Set `commit` of the source in `tool/vendor_manifest.json` to the new
   commit.
2. Run `dart tool/vendor.dart`. It needs `--webrtc-src`, so make the
   clones in step 1 of [Update on the same branch](#update-on-the-same-branch)
   at the current WebRTC pins.
3. Do steps 7 and 8 of [Update on the same branch](#update-on-the-same-branch).
