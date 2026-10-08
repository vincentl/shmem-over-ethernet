#!/bin/bash
# The adversarial read's F1: a fetch-add behind a put stream, the base (3ee3357)
# against filling the room (8a0e2fd), both with the unsent-queue experiment, at
# the kernel's queue uncapped and capped at 16 KiB, alternating three times.
# The fill tree in ~/osoe, the base in ~/osoe/build/base-src. Usage: base-fill.sh FILL BASE OUTDIR.
set -u
FILL=$1 BASE=$2 OUT=$3
mkdir -p $OUT; LOG=$OUT/run.txt; : > $LOG
echo "start $(date +%H:%M:%S)" >> $LOG
rsync -a --delete --exclude=/build --exclude=/.git "$FILL"/ pi-login:osoe/ || { echo "rsync failed" >> $LOG; exit 1; }
ssh pi-login 'rm -rf osoe/build/ladder osoe/build/base-src && mkdir -p osoe/build/ladder osoe/build/base-src' || exit 1
rsync -a --exclude=/build --exclude=/.git "$BASE"/ pi-login:osoe/build/base-src/ || { echo "rsync base failed" >> $LOG; exit 1; }
scp -q <repo>/build/scratch/ladder/hol.c pi-login:osoe/build/ladder/hol.c || exit 1
ssh pi-login 'cd osoe && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > build/ladder/build.txt 2>&1; echo "fill built: $?"
  build/release/tools/wrapper/oshcc -O2 -o build/ladder/hol-fill build/ladder/hol.c; echo "hol-fill: $?"
  cd build/base-src && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > ../ladder/build-base.txt 2>&1; echo "base built: $?"
  build/release/tools/wrapper/oshcc -O2 -o ../ladder/hol-base ../ladder/hol.c; echo "hol-base: $?"' >> $LOG
for i in 1 2 3; do
  for lowat in 0 16384; do
    for tree in base fill; do
      ssh pi-login "cd osoe && OSOE_EXPERIMENT_NOTSENT_LOWAT=$lowat srun -K1 --cpu-bind=none -N2 --ntasks-per-node=2 build/ladder/hol-$tree 10 > build/ladder/$tree-lowat-$lowat-$i.txt 2>&1; echo \"$tree-lowat-$lowat-$i: \$?\"" >> $LOG
    done
  done
done
scp -q 'pi-login:osoe/build/ladder/*.txt' $OUT/
echo "end $(date +%H:%M:%S)" >> $LOG
