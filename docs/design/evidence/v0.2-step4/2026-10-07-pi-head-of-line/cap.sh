#!/bin/bash
# The cap on each connection's unsent bytes, as built: a fetch-add behind a put
# stream, the tree as given (its cap from the measured link, no experiment's
# setting), three runs; and SHMEM_DEBUG's line for the cap the leads chose.
# Usage: cap.sh TREE OUTDIR (from the Mac).
set -u
TREE=$1 OUT=$2
mkdir -p $OUT; LOG=$OUT/run.txt; : > $LOG
echo "start $(date +%H:%M:%S)" >> $LOG
rsync -a --delete --exclude=/build --exclude=/.git "$TREE"/ pi-login:osoe/ || { echo "rsync failed" >> $LOG; exit 1; }
ssh pi-login 'rm -rf osoe/build/ladder && mkdir -p osoe/build/ladder' || exit 1
scp -q /Users/vincent/shmem-over-ethernet/build/scratch/ladder/hol.c /Users/vincent/shmem-over-ethernet/build/scratch/ladder/hol-many.c pi-login:osoe/build/ladder/ || exit 1
ssh pi-login 'cd osoe && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > build/ladder/build.txt 2>&1; echo "built: $?"
  build/release/tools/wrapper/oshcc -O2 -o build/ladder/hol build/ladder/hol.c; echo "hol: $?"
  build/release/tools/wrapper/oshcc -O2 -o build/ladder/hol-many build/ladder/hol-many.c; echo "hol-many: $?"' >> $LOG
ssh pi-login "cd osoe && SHMEM_DEBUG=1 srun -K1 --cpu-bind=none -N2 --ntasks-per-node=1 build/ladder/hol 1 2>&1 | grep -E 'unsent|link' > build/ladder/debug.txt; echo debug: \$?" >> $LOG
for i in 1 2 3; do
  ssh pi-login "cd osoe && srun -K1 --cpu-bind=none -N2 --ntasks-per-node=2 build/ladder/hol 10 > build/ladder/cap-$i.txt 2>&1; echo \"cap-$i: \$?\"" >> $LOG
  ssh pi-login "cd osoe && srun -K1 --cpu-bind=none -N2 --ntasks-per-node=4 build/ladder/hol-many 10 > build/ladder/cap-many-$i.txt 2>&1; echo \"cap-many-$i: \$?\"" >> $LOG
done
scp -q 'pi-login:osoe/build/ladder/*.txt' $OUT/
echo "end $(date +%H:%M:%S)" >> $LOG
