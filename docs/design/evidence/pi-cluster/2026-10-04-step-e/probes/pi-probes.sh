#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The second part's adversarial read's two probes on the Pi cluster at 4 x 4,
# step E (team-scans, the piece rule in elements) against step D (main at
# 158c13e, already built under ~/osoe/build/compare/before by pi.sh):
#   scan-odd   counts either side of a multiple of D, so that a D + 1th piece
#              would show as 1.7 to 1.9 times the even count's time
#   scan-skew  a 2 KiB scan (one piece on both trees) with nobody late, world
#              rank 5 (a member with children) late, the root late, a leaf late
# Three rounds, alternating D and E. Touches nothing on the Pi outside ~/osoe.
# Usage: pi-probes.sh E_TREE (from the Mac, after nothing else runs on the Pi).
set -u
AFTER=$1
OUT=<repo>/build/scratch/e-measure/pi-probes
PROBES=$(cd "$(dirname "$0")" && pwd)   # scan-odd.c and scan-skew.c, beside this script
mkdir -p $OUT
LOG=$OUT/run.txt
: > $LOG
echo "start $(date +%H:%M:%S)" >> $LOG
ssh pi-login 'test -x osoe/build/compare/before/build/release/tools/wrapper/oshcc' \
  || { echo "D's build is missing" >> $LOG; exit 1; }
rsync -a --delete --exclude=/build --exclude=/.git "$AFTER"/ pi-login:osoe/ || { echo "rsync failed" >> $LOG; exit 1; }
scp -q $PROBES/scan-odd.c $PROBES/scan-skew.c pi-login:osoe/build/compare/ || exit 1
ssh pi-login 'cd osoe && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > build/compare/after-build.txt 2>&1; echo "after built: $?"
  for p in scan-odd scan-skew; do
    build/compare/before/build/release/tools/wrapper/oshcc -O2 -o build/compare/$p-before build/compare/$p.c; echo "before $p: $?"
    build/release/tools/wrapper/oshcc -O2 -o build/compare/$p-after build/compare/$p.c; echo "after $p: $?"
  done' >> $LOG
for i in 1 2 3; do
  for side in before after; do
    ssh pi-login "cd osoe && srun -K1 --cpu-bind=none -N4 --ntasks-per-node=4 build/compare/scan-odd-$side 300 > build/compare/odd-$side-$i.txt 2>&1; echo \"odd $side $i: \$?\"
      : > build/compare/skew-$side-$i.txt
      for args in '256 -1 0 300' '256 5 1000 300' '256 0 1000 300' '256 15 1000 300'; do
        srun -K1 --cpu-bind=none -N4 --ntasks-per-node=4 build/compare/scan-skew-$side \$args >> build/compare/skew-$side-$i.txt 2>&1
      done; echo \"skew $side $i: \$?\"" >> $LOG
  done
done
scp -q 'pi-login:osoe/build/compare/odd-*.txt' 'pi-login:osoe/build/compare/skew-*.txt' $OUT/
echo "end $(date +%H:%M:%S)" >> $LOG
