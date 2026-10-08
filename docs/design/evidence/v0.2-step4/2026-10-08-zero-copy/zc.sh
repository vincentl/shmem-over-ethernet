#!/bin/bash
# v0.2 step 4 part 0 on the Pi: the zero-copy send, off and on, alternating -
# (a) a 1 MiB put stream between two nodes, each node's agent CPU over it
# (putstream-cpu.c) and the sending node's counters (osoe-stat at the window's
# start and end); (b) osoe-bench's latency and atomic lines at 2 x 1, off, on at
# the default threshold, and on for every send of 64 bytes or more.
# Usage: zc.sh TREE OUTDIR (from the Mac).
set -u
TREE=$1 OUT=$2
mkdir -p $OUT; LOG=$OUT/run.txt; : > $LOG
echo "start $(date +%H:%M:%S)" >> $LOG
rsync -a --delete --exclude=/build --exclude=/.git "$TREE"/ pi-login:osoe/ || { echo "rsync failed" >> $LOG; exit 1; }
ssh pi-login 'rm -rf osoe/build/ladder && mkdir -p osoe/build/ladder' || exit 1
scp -q <repo>/build/scratch/ladder/putstream-cpu.c pi-login:osoe/build/ladder/ || exit 1
ssh pi-login 'cd osoe && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > build/ladder/build.txt 2>&1; echo "built: $?"
  build/release/tools/wrapper/oshcc -O2 -o build/ladder/putstream-cpu build/ladder/putstream-cpu.c; echo "putstream-cpu: $?"' >> $LOG
for i in 1 2 3; do
  for zc in off on; do
    ssh pi-login "cd osoe && OSOE_WIRE_ZERO_COPY=$zc OSOE_PUTSTREAM_STAT=\$PWD/build/release/tools/stat/osoe-stat OSOE_PUTSTREAM_OUT=\$PWD/build/ladder/stat-$zc-$i srun -K1 --cpu-bind=none -N2 --ntasks-per-node=1 build/ladder/putstream-cpu 1048576 20 > build/ladder/stream-$zc-$i.txt 2>&1; echo \"stream-$zc-$i: \$?\"" >> $LOG
  done
done
for i in 1 2 3; do
  for zc in off on on-64; do
    env=""; case $zc in off) env="OSOE_WIRE_ZERO_COPY=off";; on) env="OSOE_WIRE_ZERO_COPY=on";; on-64) env="OSOE_WIRE_ZERO_COPY=on OSOE_WIRE_ZERO_COPY_BYTES=64";; esac
    ssh pi-login "cd osoe && $env srun -K1 -N2 --ntasks-per-node=1 build/release/tools/bench/osoe-bench latency atomic > build/ladder/bench-$zc-$i.txt 2>&1; echo \"bench-$zc-$i: \$?\"" >> $LOG
  done
done
scp -q 'pi-login:osoe/build/ladder/*.txt' 'pi-login:osoe/build/ladder/stat-*' $OUT/
echo "end $(date +%H:%M:%S)" >> $LOG
