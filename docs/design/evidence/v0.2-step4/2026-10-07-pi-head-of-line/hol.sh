#!/bin/bash
# v0.2 step 4 part 0, item 10 of 2026-10-07: on the Pi, a fetch-add's latency
# behind a put stream on the same connection, at connection buffers of 128, 256
# and 512 KiB, alternating three times; the branch's tree, release, in ~/osoe.
# Usage: hol.sh TREE OUTDIR (from the Mac).
set -u
TREE=$1 OUT=$2
mkdir -p $OUT; LOG=$OUT/run.txt; : > $LOG
echo "start $(date +%H:%M:%S)" >> $LOG
rsync -a --delete --exclude=/build --exclude=/.git "$TREE"/ pi-login:osoe/ || { echo "rsync failed" >> $LOG; exit 1; }
ssh pi-login 'rm -rf osoe/build/ladder && mkdir -p osoe/build/ladder' || exit 1
scp -q /Users/vincent/shmem-over-ethernet/build/scratch/ladder/hol.c pi-login:osoe/build/ladder/hol.c || exit 1
ssh pi-login 'cd osoe && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > build/ladder/build.txt 2>&1; echo "built: $?"
  build/release/tools/wrapper/oshcc -O2 -o build/ladder/hol build/ladder/hol.c; echo "hol: $?"' >> $LOG
for i in 1 2 3; do
  for buffer in 131072 262144 524288; do
    ssh pi-login "cd osoe && OSOE_AM_BUFFER=$buffer srun -K1 --cpu-bind=none -N2 --ntasks-per-node=2 build/ladder/hol 10 > build/ladder/hol-$buffer-$i.txt 2>&1; echo \"hol-$buffer-$i: \$?\"" >> $LOG
  done
done
scp -q 'pi-login:osoe/build/ladder/*.txt' $OUT/
echo "end $(date +%H:%M:%S)" >> $LOG
