#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# v0.2 step 4 part 0: two builds' put streams, alternating. Takes the VM lock.
# usage: ab.sh OUT ROUNDS SECONDS BYTES NAME... (each NAME a build in /tmp/ladder)
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
OUT=$1; ROUNDS=$2; SECONDS_EACH=$3; BYTES=$4; shift 4
S=<repo>/build/scratch/ladder
mkdir -p $OUT
for NAME in "$@"; do
  W=/tmp/ladder/$NAME/work; mkdir -p $W
  /tmp/ladder/$NAME/build/release/tools/wrapper/oshcc -O2 -g -o $W/putstream $S/putstream.c || exit 1
done
for round in $(seq $ROUNDS); do
  for NAME in "$@"; do
    R=/tmp/ladder/$NAME/build/release
    r=$($R/tools/launch/osoe-launch -n 2 -m 0,1 -t 120 -- /tmp/ladder/$NAME/work/putstream $BYTES $SECONDS_EACH 2>&1 | grep '^putstream')
    echo "round=$round build=$NAME $r" | tee -a $OUT/ab.txt
  done
done
echo DONE
