#!/bin/bash
# The pacer's mix hung once at no helpers (pacer-7): two two-PE teams each with
# one member left in the team's sync and the other gone on. main (fc99503) and
# the tip, release builds, mix for 60 s at no helpers, RUNS times each,
# alternating, the deadlock detector off: a run still going at 120 s is dumped -
# osoe-stat's summary, counters and dump of every node of the job - and the
# launcher's own timeout, at 150 s, ends it.
# usage: mix-soak.sh BASE_TAR TIP_TAR OUTDIR RUNS. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
BT=$1; TT=$2; D=$3; N=$4; mkdir -p $D; OUT=$D/soak.txt; : > $OUT
for tree in base tip; do
  T=/tmp/mix-soak-$tree; rm -rf "${T:?}"; mkdir -p $T
  if [ $tree = base ]; then tar -C $T -xf $BT; else tar -C $T -xf $TT; fi
  (cd $T && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > $D/build-$tree.txt 2>&1) || echo "BUILD FAILED $tree" >> $OUT
done
for i in $(seq 1 $N); do
  for tree in base tip; do
    T=/tmp/mix-soak-$tree; R=$T/build/release
    log=$D/mix-$tree-$i.txt
    OSOE_DEADLOCK_SECONDS=0 $R/tools/launch/osoe-launch -n 8 -m 0,0,1,1,2,2,3,3 -t 150 -- \
      $R/tools/pacer/osoe-pacer --seconds 60 mix > $log 2>&1 &
    launcher=$!
    hung=no
    for s in $(seq 1 120); do
      kill -0 $launcher 2>/dev/null || break
      sleep 1
    done
    if kill -0 $launcher 2>/dev/null; then
      hung=yes
      $R/tools/stat/osoe-stat --summary $launcher > $D/hang-$tree-$i-summary.txt 2>&1
      $R/tools/stat/osoe-stat $launcher > $D/hang-$tree-$i-counters.txt 2>&1
      $R/tools/stat/osoe-stat -d $launcher > $D/hang-$tree-$i-dump.txt 2>&1
    fi
    wait $launcher
    echo "$tree $i exit $? hung=$hung $(grep -o 'osoe-pacer failures=[0-9]*' $log | tail -1) $(date +%H:%M:%S)" >> $OUT
  done
done
rm -rf /tmp/mix-soak-base /tmp/mix-soak-tip
echo "DONE $(date +%H:%M:%S)" >> $OUT
