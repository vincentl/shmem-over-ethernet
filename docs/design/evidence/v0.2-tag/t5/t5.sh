#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# T5 in the osoe VM's Slurm (one node), as 0.1.0's sixteen cases: srun -K1 --cpu-bind=none -N1
# -n4 of ge-slurm.c, the caller at PE 0 and at PE 3, its handler 0 ms, 200 ms, 1 s and 5 s, a
# status of 0 and of 7. Each: srun's status, the handler's line, and the job's time.
#   t5.sh TREE      - TREE a release build's source tree in the VM, built
set -u
TREE=$1
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/t5-results.txt
"$TREE/build/release/tools/wrapper/oshcc" -O2 -o /var/tmp/ge-slurm "$HERE/ge-slurm.c" || exit 3
: > "$OUT"
for caller in 0 3; do
  for ms in 0 200 1000 5000; do
    for status in 0 7; do
      start=$(date +%s.%N)
      out=$(timeout 120 srun -K1 --cpu-bind=none -N1 -n4 /var/tmp/ge-slurm $caller $ms $status 2>&1)
      st=$?
      took=$(echo "$start $(date +%s.%N)" | awk '{ printf "%.2f", $2 - $1 }')
      done_line=$(printf '%s\n' "$out" | grep -c "PE $caller: handler finished after $ms ms")
      verdict=ok; { [ "$st" -eq "$status" ] && [ "$done_line" -eq 1 ]; } || verdict=FAILED
      echo "caller $caller handler ${ms} ms status $status: srun $st, handler finished $done_line, ${took} s - $verdict" | tee -a "$OUT"
    done
  done
done
echo "$(grep -c ' - ok$' "$OUT") of 16 ok" | tee -a "$OUT"
