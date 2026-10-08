#!/bin/bash
# v0.2 step 4 part 2 on the Pi, no helpers: the control wire (the branch
# control-wire) against the tree it starts from (Part 1's tip), alternating
# three times - a barrier behind a 1 MiB put stream (barrier-behind.c, the
# control plane's own number), a fetch-add behind it (hol.c), osoe-bench at
# 4 x 4 and 4 x 1, the put stream with each agent's CPU (putstream-cpu.c), the
# mailbox (osoe-pacer, 4 x 2, 20 s), and start-up at 4 x 1 and 4 x 4
# (startup.c). The tip in ~/osoe, the base in ~/osoe/build/base-src.
# Usage: pi-compare.sh TIP BASE OUTDIR (from the Mac).
set -u
TIP=$1 BASE=$2 OUT=$3
S=<repo>/build/scratch/ladder
mkdir -p $OUT; LOG=$OUT/run.txt; : > $LOG
echo "start $(date +%H:%M:%S)" >> $LOG
rsync -a --delete --exclude=/build --exclude=/.git "$TIP"/ pi-login:osoe/ || { echo "rsync failed" >> $LOG; exit 1; }
ssh pi-login 'rm -rf osoe/build/ladder osoe/build/base-src && mkdir -p osoe/build/ladder osoe/build/base-src' || exit 1
rsync -a --exclude=/build --exclude=/.git "$BASE"/ pi-login:osoe/build/base-src/ || exit 1
scp -q $S/hol.c $S/putstream-cpu.c $S/part2/barrier-behind.c $S/part2/startup.c pi-login:osoe/build/ladder/ || exit 1
ssh pi-login 'cd osoe && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > build/ladder/build.txt 2>&1; echo "tip built: $?"
  for p in hol putstream-cpu barrier-behind startup; do build/release/tools/wrapper/oshcc -O2 -o build/ladder/$p-tip build/ladder/$p.c; echo "$p-tip: $?"; done
  cd build/base-src && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > ../ladder/build-base.txt 2>&1; echo "base built: $?"
  for p in hol putstream-cpu barrier-behind startup; do build/release/tools/wrapper/oshcc -O2 -o ../ladder/$p-base ../ladder/$p.c; echo "$p-base: $?"; done' >> $LOG
for i in 1 2 3; do
  for tree in base tip; do
    if [ $tree = tip ]; then root=.; else root=build/base-src; fi
    run() { ssh pi-login "cd osoe && $2 > build/ladder/$1-$tree-$i.txt 2>&1; echo \"$1-$tree-$i: \$?\"" >> $LOG; }
    run barrier "srun -K1 --cpu-bind=none -N2 --ntasks-per-node=2 build/ladder/barrier-behind-$tree 2000"
    run hol "srun -K1 --cpu-bind=none -N2 --ntasks-per-node=2 build/ladder/hol-$tree 10"
    for shape in 4 1; do
      run bench-4x$shape "srun -K1 -N4 --ntasks-per-node=$shape $root/build/release/tools/bench/osoe-bench"
    done
    run stream "OSOE_PUTSTREAM_STAT=\$PWD/$root/build/release/tools/stat/osoe-stat OSOE_PUTSTREAM_OUT=\$PWD/build/ladder/stat-$tree-$i srun -K1 --cpu-bind=none -N2 --ntasks-per-node=1 build/ladder/putstream-cpu-$tree 1048576 20"
    run mailbox "srun -K1 -N4 --ntasks-per-node=2 $root/build/release/tools/pacer/osoe-pacer --seconds 20 mailbox"
    for shape in 1 4; do
      run startup-4x$shape "srun -K1 -N4 --ntasks-per-node=$shape build/ladder/startup-$tree"
    done
  done
done
scp -q 'pi-login:osoe/build/ladder/*.txt' $OUT/
scp -qr 'pi-login:osoe/build/ladder/stat-*' $OUT/ 2>/dev/null
echo "end $(date +%H:%M:%S)" >> $LOG
