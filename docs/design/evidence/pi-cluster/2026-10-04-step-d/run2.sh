#!/bin/bash
# The 2026-09-28 records' tree (b0e0b53) against main with D, on the Pi at
# 4 x 4, three runs each alternating: did the code or the cluster change since
# the records? Touches nothing on the Pi outside ~/osoe. From the Mac.
set -u
OUT=<repo>/build/scratch/merge-d/pi
LOG=$OUT/run2.txt
: > $LOG
echo "start $(date +%H:%M:%S)" >> $LOG
ssh pi-login 'rm -rf osoe/build/compare/record && mkdir -p osoe/build/compare/record' || exit 1
scp -q $OUT/record-b0e0b53.tar pi-login:osoe/build/compare/record.tar || exit 1
ssh pi-login 'cd osoe/build/compare/record && tar -xf ../record.tar && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > ../record-build.txt 2>&1; echo "record built: $?"' >> $LOG
ssh pi-login 'uname -r; cat /proc/cmdline | cut -c1-200' >> $LOG
for i in 1 2 3; do
  for side in record after; do
    if [ $side = record ]; then B=build/compare/record/build/release/tools/bench/osoe-bench
    else B=build/release/tools/bench/osoe-bench; fi
    ssh pi-login "cd osoe && srun -K1 -N4 --ntasks-per-node=4 $B > build/compare/bench2-$side-$i.txt 2>&1; echo \"$side $i: \$?\"" >> $LOG
  done
done
scp -q 'pi-login:osoe/build/compare/bench2-*.txt' $OUT/
echo "end $(date +%H:%M:%S)" >> $LOG
