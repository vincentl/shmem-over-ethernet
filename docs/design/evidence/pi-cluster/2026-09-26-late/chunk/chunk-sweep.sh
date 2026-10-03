#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Would a 16 KiB OSOE_AM_CHUNK cost anything? (runbook §11 item 2, 2026-09-26
# late). osoe-bench and OSU's fcollect, broadcast and put bandwidth at the
# default chunk and at 16 KiB, alternating, on main a4ca6c4. Outputs in ~/pi-results-chunk.
set -u
O=~/pi-results-chunk; mkdir -p $O; : > $O/summary.txt
say() { echo "$(date +%T) $*" | tee -a $O/summary.txt; }
R=~/shmem-over-ethernet; B=$R/build/release/tools/bench/osoe-bench
U=~/osoe-corpus/osu/c/openshmem
for round in a b; do
  for chunk in default 16384; do
    if [ $chunk = default ]; then E=OSOE_NOTHING=1; else E=OSOE_AM_CHUNK=$chunk; fi
    t=$chunk-$round
    timeout 600 env $E srun -K1 -N2 --ntasks-per-node=1 -c4 $B > $O/bench-2x1-$t.txt 2>&1; say "bench 2x1 $t exit $?"
    timeout 600 env $E srun -K1 -N4 --ntasks-per-node=4 $B > $O/bench-4x4-$t.txt 2>&1; say "bench 4x4 $t exit $?"
    cd $U
    timeout 300 env $E srun -K1 -N2 --ntasks-per-node=1 ./osu_oshm_put_bw > $O/put-bw-2x1-$t.txt 2>&1; say "put_bw $t exit $?"
    for shape in "-N4 --ntasks-per-node=1" "-N4 --ntasks-per-node=4"; do
      s=$(echo $shape | tr -d ' -' | sed 's/ntaskspernode=/x/')
      for p in fcollect broadcast; do
        timeout 600 env $E srun -K1 $shape ./osu_oshm_$p -m :1048576 > $O/$p-$s-$t.txt 2>&1; say "$p $s $t exit $?"
      done
    done
    cd - > /dev/null
  done
done
say DONE
