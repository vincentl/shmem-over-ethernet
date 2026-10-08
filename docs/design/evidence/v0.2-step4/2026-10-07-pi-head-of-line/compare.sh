#!/bin/bash
# v0.2 step 4 part 0 on the Pi: the tip of the ladder (fill the room, the
# kernel's unsent queue capped by the link, 256 KiB buffers) against the tree
# before it (3ee3357): osoe-bench at 4 x 4 and 4 x 1, alternating three times;
# then the head-of-line probes at the tip. The tip in ~/osoe, the base in
# ~/osoe/build/base-src. Usage: compare.sh TIP BASE OUTDIR (from the Mac).
set -u
TIP=$1 BASE=$2 OUT=$3
mkdir -p $OUT; LOG=$OUT/run.txt; : > $LOG
echo "start $(date +%H:%M:%S)" >> $LOG
rsync -a --delete --exclude=/build --exclude=/.git "$TIP"/ pi-login:osoe/ || { echo "rsync failed" >> $LOG; exit 1; }
ssh pi-login 'rm -rf osoe/build/ladder osoe/build/base-src && mkdir -p osoe/build/ladder osoe/build/base-src' || exit 1
rsync -a --exclude=/build --exclude=/.git "$BASE"/ pi-login:osoe/build/base-src/ || exit 1
scp -q <repo>/build/scratch/ladder/hol.c <repo>/build/scratch/ladder/hol-many.c pi-login:osoe/build/ladder/ || exit 1
ssh pi-login 'cd osoe && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > build/ladder/build.txt 2>&1; echo "tip built: $?"
  build/release/tools/wrapper/oshcc -O2 -o build/ladder/hol build/ladder/hol.c; echo "hol: $?"
  build/release/tools/wrapper/oshcc -O2 -o build/ladder/hol-many build/ladder/hol-many.c; echo "hol-many: $?"
  cd build/base-src && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > ../ladder/build-base.txt 2>&1; echo "base built: $?"' >> $LOG
for i in 1 2 3; do
  for tree in base tip; do
    if [ $tree = tip ]; then bench=build/release/tools/bench/osoe-bench; else bench=build/base-src/build/release/tools/bench/osoe-bench; fi
    for shape in 4 1; do
      ssh pi-login "cd osoe && srun -K1 -N4 --ntasks-per-node=$shape $bench > build/ladder/bench-4x$shape-$tree-$i.txt 2>&1; echo \"bench-4x$shape-$tree-$i: \$?\"" >> $LOG
    done
  done
done
for i in 1 2 3; do
  ssh pi-login "cd osoe && srun -K1 --cpu-bind=none -N2 --ntasks-per-node=2 build/ladder/hol 10 > build/ladder/hol-tip-$i.txt 2>&1; echo \"hol-tip-$i: \$?\"" >> $LOG
  ssh pi-login "cd osoe && srun -K1 --cpu-bind=none -N2 --ntasks-per-node=4 build/ladder/hol-many 10 > build/ladder/hol-many-tip-$i.txt 2>&1; echo \"hol-many-tip-$i: \$?\"" >> $LOG
done
scp -q 'pi-login:osoe/build/ladder/*.txt' $OUT/
echo "end $(date +%H:%M:%S)" >> $LOG
