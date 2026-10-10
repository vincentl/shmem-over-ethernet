#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# v0.2 step 2 Part 2 on the Pi, the scans' fix: a scan's fan-out under link is
# 0.1.0's 4 rather than the ceiling's 8. The branch's tree in ~/osoe, release,
# the e-band probe rebuilt with its oshcc; then, alternating three times, link
# against fixed at 0.1.0's defaults (k 4, D 2), e-band (reduce, broadcast,
# scan, 8 B to 1 MiB) at 4 x 4 and 4 x 1. Usage: part2-scan.sh TREE OUTDIR.
set -u
TREE=$1 OUT=$2
mkdir -p $OUT; LOG=$OUT/run.txt; : > $LOG
echo "start $(date +%H:%M:%S)" >> $LOG
rsync -a --delete --exclude=/build --exclude=/.git "$TREE"/ pi-login:osoe/ || { echo "rsync failed" >> $LOG; exit 1; }
ssh pi-login 'cd osoe && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > build/v02s2/build-scan.txt 2>&1; echo "built: $?"
  OSHCC=$HOME/osoe/build/release/tools/wrapper/oshcc
  $OSHCC -O2 -o build/v02s2/e-band build/v02s2/e-band.c; echo "e-band: $?"' >> $LOG
FIXED="OSOE_COLLECTIVE_SHAPE=fixed OSOE_COLLECTIVE_FANOUT=4 OSOE_COLLECTIVE_PIPELINE=2"
run() {
  ssh pi-login "cd osoe && env $2 $3 > build/v02s2/$1.txt 2>&1; echo \"$1: \$?\"" >> $LOG
}
for i in 1 2 3; do
  for side in link fixed; do
    env="OSOE_COLLECTIVE_SHAPE=link"; [ $side = fixed ] && env="$FIXED"
    run scan-band-4x4-$side-$i "$env" "srun -K1 --cpu-bind=none -N4 --ntasks-per-node=4 build/v02s2/e-band"
    run scan-band-4x1-$side-$i "$env" "srun -K1 --cpu-bind=none -N4 --ntasks-per-node=1 build/v02s2/e-band"
  done
done
scp -q 'pi-login:osoe/build/v02s2/scan-band-*.txt' $OUT/ && echo "fetched" >> $LOG
echo "end $(date +%H:%M:%S)" >> $LOG
