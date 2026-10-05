#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Step E against step D on the Pi cluster at 4 x 4 (the brief's Part 2): main
# (D and the review's fixes) under ~/osoe/build/compare/before and step E's tree
# in ~/osoe, both release builds; osoe-bench and e-band three times each,
# alternating; osoe-pacer --quick once each. Touches nothing on the Pi outside
# ~/osoe. Usage: pi.sh E_TREE D_TAR RUN (from the Mac).
set -u
AFTER=$1 BEFORE=$2 RUN=$3
OUT=<repo>/build/scratch/e-measure/pi-$RUN
mkdir -p $OUT
LOG=$OUT/run.txt
: > $LOG
echo "start $(date +%H:%M:%S)" >> $LOG
rsync -a --delete --exclude=/build --exclude=/.git "$AFTER"/ pi-login:osoe/ || { echo "rsync failed" >> $LOG; exit 1; }
ssh pi-login 'rm -rf osoe/build/compare && mkdir -p osoe/build/compare/before' || exit 1
scp -q "$BEFORE" pi-login:osoe/build/compare/before.tar || exit 1
scp -q <repo>/build/scratch/e-measure/e-band.c pi-login:osoe/build/compare/e-band.c || exit 1
ssh pi-login 'cd osoe/build/compare/before && tar -xf ../before.tar && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > ../before-build.txt 2>&1; echo "before built: $?"; build/release/tools/wrapper/oshcc -O2 -o ../e-band-before ../e-band.c; echo "before probe: $?"' >> $LOG
ssh pi-login 'cd osoe && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > build/compare/after-build.txt 2>&1; echo "after built: $?"; build/release/tools/wrapper/oshcc -O2 -o build/compare/e-band-after build/compare/e-band.c; echo "after probe: $?"' >> $LOG
ssh pi-login 'cd osoe && srun -N4 --ntasks-per-node=1 build/release/tools/probe/probe 2>&1 | tail -2' >> $LOG
for i in 1 2 3; do
  for side in before after; do
    if [ $side = before ]; then B=build/compare/before/build/release/tools/bench/osoe-bench
    else B=build/release/tools/bench/osoe-bench; fi
    ssh pi-login "cd osoe && srun -K1 -N4 --ntasks-per-node=4 $B > build/compare/bench-$side-$i.txt 2>&1; echo \"bench $side $i: \$?\"; srun -K1 --cpu-bind=none -N4 --ntasks-per-node=4 build/compare/e-band-$side > build/compare/band-$side-$i.txt 2>&1; echo \"band $side $i: \$?\"" >> $LOG
  done
done
for side in before after; do
  if [ $side = before ]; then P=build/compare/before/build/release/tools/pacer/osoe-pacer
  else P=build/release/tools/pacer/osoe-pacer; fi
  ssh pi-login "cd osoe && srun -K1 --cpu-bind=none -N4 --ntasks-per-node=4 $P --quick > build/compare/pacer-$side.txt 2>&1; echo \"pacer $side: \$? \$(tail -1 build/compare/pacer-$side.txt)\"" >> $LOG
done
scp -q 'pi-login:osoe/build/compare/*.txt' $OUT/
echo "end $(date +%H:%M:%S)" >> $LOG
