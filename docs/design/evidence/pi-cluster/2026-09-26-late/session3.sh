#!/bin/bash
# The Pi cluster's third session (2026-09-26 late evening), docs/pi-cluster.md
# §11 items 1, 2 and 4, on main a4ca6c4: build, probe, tier 2, SOS's context and
# scan programs, F1's fcollect at 4 x 2 with 16 KiB blocks (TCP counters read on
# every node before and after each run), F2's small-message sweep at k = 2 and
# k = 4. Outputs in ~/pi-results-3.
set -u
R=~/shmem-over-ethernet; W=~/osoe-corpus; O=~/pi-results-3
mkdir -p $O; : > $O/summary.txt
say() { echo "$(date +%T) $*" | tee -a $O/summary.txt; }
cd $R
cmake --preset release > $O/build.txt 2>&1 && cmake --build build/release -j4 >> $O/build.txt 2>&1
say "build exit $?"
srun -N4 --ntasks-per-node=1 build/release/tools/probe/probe > $O/probe.txt 2>&1
say "probe exit $?"
srun -N1 -n1 -c4 ctest --test-dir build/release -L tier2 -E '^model\.' -j4 --timeout 300 > $O/tier2.txt 2>&1
say "tier2: $(grep -E 'tests passed' $O/tier2.txt)"

OSHCC=$R/build/release/tools/wrapper/oshcc
S=$O/sos; mkdir -p $S; cd $S
SOS=$R/../osoe-corpus/sos
for p in spec-example/shmem_team_context unit/many-ctx unit/shmem_ctx_get_team \
         spec-example/shmem_ctx_pipelined_reduce; do
  mkdir -p $(dirname $p)
  $OSHCC -O1 -I$SOS/include $SOS/$p.c -o $p -lm -pthread > $p.build 2>&1 || { say "$p: build failed"; continue; }
  timeout 120 srun -K1 -N4 --ntasks-per-node=2 ./$p > $p.out 2>&1 < /dev/null
  say "$p 4x2 exit $?"
done
for p in in_scan ex_scan scan_in_place; do
  sed -e 's/shmemx_/shmem_/g' -e '/#include <shmemx.h>/d' $SOS/shmemx/$p.c > $p.c
  $OSHCC -O1 -I$SOS/include $p.c -o $p > $p.build 2>&1 || { say "$p: build failed"; continue; }
  for shape in "-N4 --ntasks-per-node=2" "-N4 --ntasks-per-node=2 --distribution=cyclic" "-N4 --ntasks-per-node=4"; do
    timeout 120 srun -K1 $shape ./$p > $p.out 2>&1 < /dev/null
    say "$p [$shape] exit $?"
  done
done

cd $W/osu/c/openshmem
counters() { srun -N4 --ntasks-per-node=1 sh -c 'echo "$(hostname) $(nstat -az TcpRetransSegs TcpExtTCPTimeouts | awk "NR>2{printf \"%s=%s \", \$1, \$2}")"' 2>/dev/null | sort; }
f1() {   # label, then srun shape and env
  local label=$1; shift
  counters > $O/f1-$label.before
  env "$@" > /dev/null   # validates env words
  timeout 300 env "$@" srun -K1 -N4 --ntasks-per-node=2 ./osu_oshm_fcollect -m 16384:16384 -i 200 > $O/f1-$label.txt 2>&1
  local status=$?
  counters > $O/f1-$label.after
  say "F1 $label exit $status: $(grep -E '^16384' $O/f1-$label.txt | tr -s ' ')"
}
f1 base OSOE_NOTHING=1
f1 again OSOE_NOTHING=1
f1 pipeline1 OSOE_COLLECTIVE_PIPELINE=1
f1 chunk16k OSOE_AM_CHUNK=16384
f1 fanout2 OSOE_COLLECTIVE_FANOUT=2
for size in 8192 32768; do
  timeout 300 srun -K1 -N4 --ntasks-per-node=2 ./osu_oshm_fcollect -m $size:$size -i 200 > $O/f1-size-$size.txt 2>&1
  say "F1 size $size: $(grep -E "^$size" $O/f1-size-$size.txt | tr -s ' ')"
done

for k in 2 4; do
  for p in broadcast reduce; do
    timeout 600 env OSOE_COLLECTIVE_FANOUT=$k srun -K1 -N4 --ntasks-per-node=1 ./osu_oshm_$p -m :65536 > $O/f2-$p-k$k.txt 2>&1
    say "F2 $p k=$k exit $?"
  done
done
say DONE
