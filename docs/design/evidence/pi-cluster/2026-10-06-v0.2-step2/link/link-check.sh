#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Step 2's link measurement on the Pi: the branch's tree into ~/osoe (nothing
# outside it), a release build, then the link each start measures and what it
# cost the start - ten starts at 4 x 1, five at 4 x 4 - read from SHMEM_DEBUG's
# start-up lines. Usage: link-check.sh TREE OUTDIR (from the Mac).
set -u
TREE=$1 OUT=$2
mkdir -p $OUT; LOG=$OUT/run.txt; : > $LOG
echo "start $(date +%H:%M:%S)" >> $LOG
rsync -a --delete --exclude=/build --exclude=/.git "$TREE"/ pi-login:osoe/ || { echo "rsync failed" >> $LOG; exit 1; }
ssh pi-login 'cd osoe && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > build/link-build.txt 2>&1; echo "built: $?"; grep -c "warning:" build/link-build.txt' >> $LOG
P=build/release/shmem/tests/shmem_test_barrier
for shape in "1 10" "4 5"; do
  set -- $shape
  per=$1 runs=$2
  for i in $(seq $runs); do
    ssh pi-login "cd osoe && SHMEM_DEBUG=1 srun -K1 -N4 --ntasks-per-node=$per $P 2>&1 | grep 'link measured' | sort | head -$((4 * per))" > $OUT/run-$per-$i.txt 2>&1
    echo "4x$per run $i: $(grep -c 'link measured' $OUT/run-$per-$i.txt) lines; $(grep 'PE 0 of' $OUT/run-$per-$i.txt | sed 's/.*; link measured: //')" >> $LOG
  done
done
echo "end $(date +%H:%M:%S)" >> $LOG
