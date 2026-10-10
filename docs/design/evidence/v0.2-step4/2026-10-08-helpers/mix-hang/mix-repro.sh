#!/bin/bash
# The pacer's mix hung at no helpers (pacer-7): a team sync completed on one
# node of a two-PE team and not the other. Is it the tip's or older? main
# (fc99503) and the tip, release builds, mix for 60 s at no helpers, RUNS times
# each, alternating; each run's lines kept. usage: mix-repro.sh BASE_TAR TIP_TAR
# OUTDIR RUNS. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
BT=$1; TT=$2; D=$3; N=$4; mkdir -p $D; OUT=$D/repro.txt; : > $OUT
for tree in base tip; do
  T=/tmp/mix-repro-$tree; rm -rf "${T:?}"; mkdir -p $T
  if [ $tree = base ]; then tar -C $T -xf $BT; else tar -C $T -xf $TT; fi
  (cd $T && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > $D/build-$tree.txt 2>&1) || echo "BUILD FAILED $tree" >> $OUT
done
for i in $(seq 1 $N); do
  for tree in base tip; do
    T=/tmp/mix-repro-$tree
    $T/build/release/tools/launch/osoe-launch -n 8 -m 0,0,1,1,2,2,3,3 -t 180 -- $T/build/release/tools/pacer/osoe-pacer --seconds 60 mix > $D/mix-$tree-$i.txt 2>&1
    echo "$tree $i exit $? $(grep -c 'osoe: deadlock' $D/mix-$tree-$i.txt) deadlock-lines $(grep -o 'osoe-pacer failures=[0-9]*' $D/mix-$tree-$i.txt | tail -1) $(date +%H:%M:%S)" >> $OUT
  done
done
rm -rf /tmp/mix-repro-base /tmp/mix-repro-tip
echo "DONE $(date +%H:%M:%S)" >> $OUT
