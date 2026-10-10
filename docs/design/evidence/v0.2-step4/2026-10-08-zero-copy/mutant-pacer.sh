#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# A mutant's release build under osoe-pacer mix with the zero-copy send on and
# the deadlock detector off, so that a stranded wait is the launcher's timeout.
# usage: mutant-pacer.sh TAR NAME OUT SECONDS. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
TAR=$1; NAME=$2; OUT=$3; SECS=$4; mkdir -p $OUT
S=<repo>/build/scratch/ladder
T=/tmp/ladder/$NAME; rm -rf $T; mkdir -p $T; tar -C $T -xf $TAR; cd $T
cmake --preset release > $OUT/configure.txt 2>&1 && cmake --build build/release -j4 > $OUT/build.txt 2>&1 || { echo BUILD FAILED > $OUT/pacer.txt; exit 1; }
L=$T/build/release/tools/launch/osoe-launch; P=$T/build/release/tools/pacer/osoe-pacer
for i in 1 2 3; do
  start=$(date +%s)
  OSOE_WIRE_ZERO_COPY=on OSOE_WIRE_ZERO_COPY_BYTES=1024 OSOE_DEADLOCK_SECONDS=0 $L -n 8 -m 0,0,1,1,2,2,3,3 -t $((SECS + 60)) -- $P --seconds $SECS mix > $OUT/mix-$i.txt 2>&1
  echo "run=$i exit=$? seconds=$(( $(date +%s) - start )) $(tail -1 $OUT/mix-$i.txt)" >> $OUT/pacer.txt
done
echo DONE >> $OUT/pacer.txt
