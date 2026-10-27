#!/usr/bin/env bash
# Samples the stacks of the simulator's Runner (the example app) 90 s and
# 400 s after each launch, into <dir>, while the job runs. On CI the app
# sometimes stops 2 s after launch, before the Dart VM starts (no VM service
# in 600 s, three attempts; 2026-10-27), and does not locally; the samples
# show where it waits. Start it in the background before ios-flutter-test.
#
#   tool/ci/ios_sample_runner.sh <dir> &
set -uo pipefail
dir=$1
mkdir -p "$dir"
while true; do
  for pid in $(pgrep -f '/Runner\.app/Runner$|/Runner\.app/Runner ' || true); do
    age=$(ps -o etime= -p "$pid" | awk -F'[-:]' '{ s = 0; for (i = 1; i <= NF; i++) s = s * 60 + $i; print s }')
    [[ -n "$age" ]] || continue
    for at in ${SAMPLE_AT:-90 400}; do
      out="$dir/sample_${pid}_${at}s.txt"
      if (( age >= at )) && [[ ! -e "$out" ]]; then
        {
          ps -o pid,etime,stat,%cpu,command -p "$pid"
          sample "$pid" 3
        } > "$out" 2>&1
      fi
    done
  done
  sleep 10
done
