#!/bin/bash
# Step D's merge point on the Pi cluster (the brief: the runbook's barrier and
# 8-byte collective lines, D changing no data path): main with D in ~/osoe, as
# the runbook has it, and main without D under ~/osoe/build/compare/before,
# both release builds, osoe-bench at 4 x 4 (Slurm's default binding) three
# times each, alternating. Touches nothing on the Pi outside ~/osoe.
# Usage: run.sh AFTER_TREE BEFORE_TAR   (from the Mac)
set -u
AFTER=$1 BEFORE=$2
OUT=<repo>/build/scratch/merge-d/pi
LOG=$OUT/run.txt
: > $LOG
echo "start $(date +%H:%M:%S)" >> $LOG
rsync -a --delete --exclude=/build --exclude=/.git "$AFTER"/ pi-login:osoe/ || { echo "rsync failed" >> $LOG; exit 1; }
ssh pi-login 'rm -rf osoe/build/compare && mkdir -p osoe/build/compare/before' || exit 1
scp -q "$BEFORE" pi-login:osoe/build/compare/before.tar || exit 1
ssh pi-login 'cd osoe/build/compare/before && tar -xf ../before.tar && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > ../before-build.txt 2>&1; echo "before built: $?"' >> $LOG
ssh pi-login 'cd osoe && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > build/compare/after-build.txt 2>&1; echo "after built: $?"' >> $LOG
ssh pi-login 'cd osoe && srun -N4 --ntasks-per-node=1 build/release/tools/probe/probe 2>&1 | tail -4' >> $LOG
for i in 1 2 3; do
  for side in before after; do
    if [ $side = before ]; then B=build/compare/before/build/release/tools/bench/osoe-bench
    else B=build/release/tools/bench/osoe-bench; fi
    ssh pi-login "cd osoe && srun -K1 -N4 --ntasks-per-node=4 $B > build/compare/bench-$side-$i.txt 2>&1; echo \"$side $i: \$?\"" >> $LOG
  done
done
scp -q 'pi-login:osoe/build/compare/bench-*.txt' $OUT/
echo "end $(date +%H:%M:%S)" >> $LOG
