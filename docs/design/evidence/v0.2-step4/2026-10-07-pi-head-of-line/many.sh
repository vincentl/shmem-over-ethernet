#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Three writers of fetch-adds beside one put stream on one connection, four PEs
# a node over two Pi nodes, the kernel's unsent queue capped at 16 KiB: the base
# (3ee3357), filling the room (8a0e2fd) and filling with the spare (f643837),
# each with the cap experiment, alternating three times.
# Usage: many.sh FILL SPARE BASE OUTDIR (from the Mac).
set -u
FILL=$1 SPARE=$2 BASE=$3 OUT=$4
mkdir -p $OUT; LOG=$OUT/run.txt; : > $LOG
echo "start $(date +%H:%M:%S)" >> $LOG
rsync -a --delete --exclude=/build --exclude=/.git "$FILL"/ pi-login:osoe/ || { echo "rsync failed" >> $LOG; exit 1; }
ssh pi-login 'rm -rf osoe/build/ladder osoe/build/base-src osoe/build/spare-src && mkdir -p osoe/build/ladder osoe/build/base-src osoe/build/spare-src' || exit 1
rsync -a --exclude=/build --exclude=/.git "$BASE"/ pi-login:osoe/build/base-src/ || exit 1
rsync -a --exclude=/build --exclude=/.git "$SPARE"/ pi-login:osoe/build/spare-src/ || exit 1
scp -q <repo>/build/scratch/ladder/hol-many.c pi-login:osoe/build/ladder/hol-many.c || exit 1
ssh pi-login 'cd osoe && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > build/ladder/build.txt 2>&1; echo "fill built: $?"
  build/release/tools/wrapper/oshcc -O2 -o build/ladder/many-fill build/ladder/hol-many.c; echo "many-fill: $?"
  for t in base spare; do (cd build/$t-src && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > ../ladder/build-$t.txt 2>&1; echo "$t built: $?"
    build/release/tools/wrapper/oshcc -O2 -o ../ladder/many-$t ../ladder/hol-many.c; echo "many-$t: $?"); done' >> $LOG
for i in 1 2 3; do
  for tree in base fill spare; do
    ssh pi-login "cd osoe && OSOE_EXPERIMENT_NOTSENT_LOWAT=16384 srun -K1 --cpu-bind=none -N2 --ntasks-per-node=4 build/ladder/many-$tree 10 > build/ladder/many-$tree-$i.txt 2>&1; echo \"many-$tree-$i: \$?\"" >> $LOG
  done
done
scp -q 'pi-login:osoe/build/ladder/*.txt' $OUT/
echo "end $(date +%H:%M:%S)" >> $LOG
