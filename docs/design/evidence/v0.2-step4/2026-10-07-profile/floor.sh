#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# shmem.floor's stream ratio, the README's "a fifth": RUNS runs of the test in
# each NAMEd release build, alternating. usage: floor.sh OUT RUNS NAME... Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
OUT=$1; RUNS=$2; shift 2; mkdir -p $OUT; : > $OUT/floor.txt
for run in $(seq $RUNS); do
  for NAME in "$@"; do
    B=/tmp/ladder/$NAME/build/release
    r=$($B/tools/launch/osoe-launch -n 2 -m 0,1 -t 150 -- $B/shmem/tests/shmem_test_floor 2>&1 | grep -E 'stream library/floor|stream_mib_per_s' | tail -3 | paste -sd' ' -)
    echo "run=$run build=$NAME $r" | tee -a $OUT/floor.txt
  done
done
