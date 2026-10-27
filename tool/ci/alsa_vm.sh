#!/usr/bin/env bash
# Runs the native tests and echo_probe on ALSA inside a VM, on a hosted x64
# Linux runner. The runners' Azure kernel has no sound support at all
# (CONFIG_SOUND unset; probe 2026-10-27), so this boots Ubuntu's generic
# kernel with virtme-ng under KVM, over the runner's own root file system
# (the build tree is shared), and loads snd-aloop there. A .asoundrc makes
# the default PCM play to Loopback 0,0 and record from Loopback 1,0, so the
# microphone hears the speaker.
#
#   tool/ci/alsa_vm.sh <build dir with native_test/tsnx_tests and echo_probe>
#
# Needs /dev/kvm (x64 runners have it; ARM64 runners do not).
set -euo pipefail
build=$(cd "$1" && pwd)
work=${RUNNER_TEMP:-/tmp}/alsa_vm
mkdir -p "$work"

# The generic kernel and its modules, unpacked (not installed: installing
# linux-image-generic pulls linux-firmware and rebuilds initramfs and grub,
# 7 minutes on a runner, none of which virtme-ng needs).
if ! command -v vng > /dev/null; then
  sudo apt-get install -y -q virtme-ng
fi
release=$(apt-cache depends linux-image-generic | awk '/Depends: linux-image-[0-9]/ { sub(/.*linux-image-/, ""); print; exit }')
if [[ ! -e "/boot/vmlinuz-$release" ]]; then
  (cd "$work" && apt-get download -q "linux-image-unsigned-$release" "linux-modules-$release" "linux-modules-extra-$release")
  for deb in "$work"/linux-*.deb; do sudo dpkg -x "$deb" /; done
  sudo depmod -a "$release"
fi
sudo chmod a+r "/boot/vmlinuz-$release"
sudo chmod a+rw /dev/kvm
echo "guest kernel $release"

mkdir -p "$work/home"
cat > "$work/home/.asoundrc" << 'ASOUND'
pcm.!default {
  type asym
  playback.pcm "plughw:Loopback,0,0"
  capture.pcm "plughw:Loopback,1,0"
}
ctl.!default { type hw card Loopback }
ASOUND

cat > "$work/guest.sh" << 'GUEST'
set -uo pipefail
modprobe snd-aloop || exit 10
export HOME="$ALSA_HOME"
# alsa-lib rereads its configuration on every device scan, and over the
# guest's 9p root that took 1-10 s per device selection (routes timed out);
# from tmpfs it takes about 270 ms.
mkdir -p /tmp/alsa-config && cp -a /usr/share/alsa/. /tmp/alsa-config/
export ALSA_CONFIG_DIR=/tmp/alsa-config
# The runner's pulseaudio and libasound2-plugins add config hooks (pulse.conf
# and others) that make every device scan look for a PulseAudio server; with
# none in the guest, routes hung (120 s, thousands of "Home directory not
# accessible"). Only the hw devices are under test here, so drop them (in
# the guest: /etc is an overlay).
rm -f /tmp/alsa-config/alsa.conf.d/* /etc/alsa/conf.d/*
cat /proc/asound/cards
status=0
step() {
  echo "::group::$*"
  local start=$SECONDS
  timeout 120 "$@"
  local rc=$?
  echo "exit $rc after $(( SECONDS - start )) s"
  echo "::endgroup::"
  if (( rc != 0 )); then echo "::error::$* exited with $rc"; status=1; fi
}
step "$BUILD/native_test/tsnx_tests"
step "$BUILD/echo_probe" --alsa --routes
step "$BUILD/echo_probe" --alsa --clock observe 3
# Start/stop cycles: the ALSA shutdown paths (upstream #244, our clock patch).
for _ in 1 2 3 4 5; do step "$BUILD/echo_probe" --alsa 1; done
exit $status
GUEST

# vng exits with the guest's status (a failing echo_probe failed the step in
# a probe run).
timeout 900 vng --run "/boot/vmlinuz-$release" --user root --memory 2G --cpus 2 \
  --exec "ALSA_HOME=$work/home BUILD=$build bash $work/guest.sh"
