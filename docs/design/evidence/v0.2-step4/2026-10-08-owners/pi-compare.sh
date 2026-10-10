#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# v0.2 step 4 part 1 on the Pi: owners at no helpers (the branch `owners`)
# against the tree it starts from (main 44f24bb), which must not differ:
# osoe-bench at 4 x 4 and 4 x 1, the 1 MiB put stream with each node's agent
# CPU (putstream-cpu.c), and the head-of-line probes - every one alternating
# three times. The tip in ~/osoe, the base in ~/osoe/build/base-src.
# Usage: pi-compare.sh TIP BASE OUTDIR (from the Mac).
set -u
TIP=$1 BASE=$2 OUT=$3
S=<repo>/build/scratch/ladder
mkdir -p $OUT; LOG=$OUT/run.txt; : > $LOG
echo "start $(date +%H:%M:%S)" >> $LOG
rsync -a --delete --exclude=/build --exclude=/.git "$TIP"/ pi-login:osoe/ || { echo "rsync failed" >> $LOG; exit 1; }
ssh pi-login 'rm -rf osoe/build/ladder osoe/build/base-src && mkdir -p osoe/build/ladder osoe/build/base-src' || exit 1
rsync -a --exclude=/build --exclude=/.git "$BASE"/ pi-login:osoe/build/base-src/ || exit 1
scp -q $S/hol.c $S/hol-many.c $S/putstream-cpu.c pi-login:osoe/build/ladder/ || exit 1
ssh pi-login 'cd osoe && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > build/ladder/build.txt 2>&1; echo "tip built: $?"
  for p in hol hol-many putstream-cpu; do build/release/tools/wrapper/oshcc -O2 -o build/ladder/$p-tip build/ladder/$p.c; echo "$p-tip: $?"; done
  cd build/base-src && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > ../ladder/build-base.txt 2>&1; echo "base built: $?"
  for p in hol hol-many putstream-cpu; do build/release/tools/wrapper/oshcc -O2 -o ../ladder/$p-base ../ladder/$p.c; echo "$p-base: $?"; done' >> $LOG
for i in 1 2 3; do
  for tree in base tip; do
    if [ $tree = tip ]; then root=.; else root=build/base-src; fi
    for shape in 4 1; do
      ssh pi-login "cd osoe && srun -K1 -N4 --ntasks-per-node=$shape $root/build/release/tools/bench/osoe-bench > build/ladder/bench-4x$shape-$tree-$i.txt 2>&1; echo \"bench-4x$shape-$tree-$i: \$?\"" >> $LOG
    done
    ssh pi-login "cd osoe && OSOE_PUTSTREAM_STAT=\$PWD/$root/build/release/tools/stat/osoe-stat OSOE_PUTSTREAM_OUT=\$PWD/build/ladder/stat-$tree-$i srun -K1 --cpu-bind=none -N2 --ntasks-per-node=1 build/ladder/putstream-cpu-$tree 1048576 20 > build/ladder/stream-$tree-$i.txt 2>&1; echo \"stream-$tree-$i: \$?\"" >> $LOG
    ssh pi-login "cd osoe && srun -K1 --cpu-bind=none -N2 --ntasks-per-node=2 build/ladder/hol-$tree 10 > build/ladder/hol-$tree-$i.txt 2>&1; echo \"hol-$tree-$i: \$?\"" >> $LOG
    ssh pi-login "cd osoe && srun -K1 --cpu-bind=none -N2 --ntasks-per-node=4 build/ladder/hol-many-$tree 10 > build/ladder/hol-many-$tree-$i.txt 2>&1; echo \"hol-many-$tree-$i: \$?\"" >> $LOG
  done
done
scp -q 'pi-login:osoe/build/ladder/*.txt' $OUT/
scp -qr 'pi-login:osoe/build/ladder/stat-*' $OUT/ 2>/dev/null
echo "end $(date +%H:%M:%S)" >> $LOG
