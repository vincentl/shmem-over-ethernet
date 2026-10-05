#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Round six's scale-and-performance review asked the lead for two Pi runs:
# reduce-skew (U2: a reduction's late lead, D against E, alternating) and
# piece-edge (U1: reductions and broadcasts either side of a lane slot's
# 16,368 payload bytes, E alone). ~/osoe holds main (375973a) built, and
# ~/osoe/build/compare/before D (158c13e), from pi.sh's merge run.
set -u
D=$(cd "$(dirname "$0")" && pwd)   # the probes, beside this script
OUT=$D/out; mkdir -p $OUT; LOG=$OUT/run.txt; : > $LOG
echo "start $(date +%H:%M:%S)" >> $LOG
scp -q $D/reduce-skew.c $D/piece-edge.c pi-login:osoe/build/compare/ || exit 1
ssh pi-login 'cd osoe && for side in before after; do
    if [ $side = before ]; then O=build/compare/before/build/release/tools/wrapper/oshcc; else O=build/release/tools/wrapper/oshcc; fi
    $O -O2 -o build/compare/reduce-skew-$side build/compare/reduce-skew.c; echo "$side reduce-skew: $?"
  done
  build/release/tools/wrapper/oshcc -O2 -o build/compare/piece-edge build/compare/piece-edge.c; echo "piece-edge: $?"' >> $LOG
for i in 1 2 3; do
  for side in before after; do
    ssh pi-login "cd osoe && : > build/compare/rskew-$side-$i.txt
      for L in -1 0 1 12; do echo \"== 4x4 k=4 L=\$L\" >> build/compare/rskew-$side-$i.txt; srun -K1 --cpu-bind=none -N4 --ntasks-per-node=4 build/compare/reduce-skew-$side 512 \$L 1000 300 >> build/compare/rskew-$side-$i.txt 2>&1; done
      echo '== 4x4 k=2 L=4' >> build/compare/rskew-$side-$i.txt; OSOE_COLLECTIVE_FANOUT=2 srun -K1 --cpu-bind=none -N4 --ntasks-per-node=4 build/compare/reduce-skew-$side 512 4 1000 300 >> build/compare/rskew-$side-$i.txt 2>&1
      for L in -1 0 3; do echo \"== 4x1 k=4 L=\$L\" >> build/compare/rskew-$side-$i.txt; srun -K1 --cpu-bind=none -N4 --ntasks-per-node=1 build/compare/reduce-skew-$side 512 \$L 1000 300 >> build/compare/rskew-$side-$i.txt 2>&1; done
      echo \"rskew $side $i: done\"" >> $LOG
  done
  ssh pi-login "cd osoe && srun -K1 --cpu-bind=none -N4 --ntasks-per-node=4 build/compare/piece-edge > build/compare/edge-4x4-$i.txt 2>&1; echo \"edge 4x4 $i: \$?\"; srun -K1 --cpu-bind=none -N4 --ntasks-per-node=1 build/compare/piece-edge > build/compare/edge-4x1-$i.txt 2>&1; echo \"edge 4x1 $i: \$?\"" >> $LOG
done
scp -q 'pi-login:osoe/build/compare/rskew-*.txt' 'pi-login:osoe/build/compare/edge-*.txt' $OUT/
echo "end $(date +%H:%M:%S)" >> $LOG
