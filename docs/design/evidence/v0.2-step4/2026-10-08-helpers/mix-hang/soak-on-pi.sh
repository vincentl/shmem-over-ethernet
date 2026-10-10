#!/bin/bash
# Run on pi-login, in one ssh session held open throughout (systemd-logind's
# RemoveIPC: docs/pi-cluster.md). The pacer's mix for 60 s, 4 x 2, the tip
# (~/osoe) and main (~/osoe/build/base-src) alternating RUNS times at no
# helpers, then the tip HELPER_RUNS times at two; the deadlock detector off, so
# a hang stays to be read: a job still running at 120 s is read on every node -
# osoe-stat's summary and counters - then cancelled.
# usage: soak-on-pi.sh RUNS HELPER_RUNS
set -u
N=$1 H=$2
OUT=~/osoe/build/ladder/soak; mkdir -p $OUT; : > $OUT/soak.txt
one() {   # tree, label, environment
  tree=$1 label=$2 env=$3
  if [ $tree = tip ]; then R=~/osoe/build/release; else R=~/osoe/build/base-src/build/release; fi
  log=$OUT/mix-$label.txt
  env OSOE_DEADLOCK_SECONDS=0 $env srun -K1 -N4 --ntasks-per-node=2 -t 4 -J soak-$label \
      $R/tools/pacer/osoe-pacer --seconds 60 mix > $log 2>&1 &
  pid=$!
  hung=no
  for s in $(seq 1 120); do kill -0 $pid 2>/dev/null || break; sleep 1; done
  if kill -0 $pid 2>/dev/null; then
    hung=yes
    job=$(squeue -h -n soak-$label -o %i | head -1)
    srun --jobid=$job --overlap -N4 --ntasks-per-node=1 $R/tools/stat/osoe-stat --summary $job \
        > $OUT/hang-$label-summary.txt 2>&1
    srun --jobid=$job --overlap -N4 --ntasks-per-node=1 $R/tools/stat/osoe-stat $job \
        > $OUT/hang-$label-counters.txt 2>&1
    scancel $job
  fi
  wait $pid
  echo "$label exit $? hung=$hung $(grep -o 'osoe-pacer failures=[0-9]*' $log | tail -1) $(date +%H:%M:%S)" >> $OUT/soak.txt
}
for i in $(seq 1 $N); do
  one base base-$i ""
  one tip tip-$i ""
done
for i in $(seq 1 $H); do
  one tip tip-helpers2-$i OSOE_PROGRESS_HELPERS=2
done
echo "DONE $(date +%H:%M:%S)" >> $OUT/soak.txt
