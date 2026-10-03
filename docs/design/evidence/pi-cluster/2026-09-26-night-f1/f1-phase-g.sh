#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# F1 phase G: only the internode allgather's puts cut to 16 KiB pieces
# (OSOE_EXPERIMENT_GATHER_PIECE), the chunk left at 64 KiB for everything else.
set -u
O=~/pi-results-f1; say() { echo "$(date +%T) $*" | tee -a $O/summary.txt; }
cd ~/osoe-f1
python3 - <<'PY'
p = 'shmem/src/internode-generic.c'
s = open(p).read()
old = """    osoe_transport_put(context, receiver, gather->region, gather->offset + from,
                       gather->buffer + from, (size_t)(to - from), OSOE_TRANSPORT_MORE);"""
new = """    { /* EXPERIMENT: the allgather's puts in pieces */
      const char *e = getenv("OSOE_EXPERIMENT_GATHER_PIECE");
      uint64_t piece = e ? strtoull(e, NULL, 0) : 0;
      while (piece && to - from > piece) {
          osoe_transport_put(context, receiver, gather->region, gather->offset + from,
                             gather->buffer + from, (size_t)piece, OSOE_TRANSPORT_MORE);
          from += piece;
      }
    }
""" + old
if 'OSOE_EXPERIMENT_GATHER_PIECE' not in s:
    assert s.count(old) == 1
    s = s.replace(old, new)
    if '#include <stdlib.h>' not in s:
        s = s.replace('#include <string.h>', '#include <stdlib.h>\n#include <string.h>', 1)
    open(p, 'w').write(s)
PY
grep -c OSOE_EXPERIMENT_GATHER_PIECE shmem/src/internode-generic.c | sed 's/^/gather lines: /' >> $O/summary.txt
cmake --build build/release -j4 --target osoe > $O/build-gather.txt 2>&1; say "gather build $?"
L=~/osoe-f1/build/release/shmem/libosoe.so.0
B=~/shmem-over-ethernet/build/release/tools/bench/osoe-bench
for round in a b; do
  for cfg in OSOE_NOTHING=1 OSOE_EXPERIMENT_GATHER_PIECE=16384; do
    timeout 900 env LD_PRELOAD=$L $cfg srun -K1 -N4 --ntasks-per-node=1 ~/f1probe 24576 28672 32768 36864 > $O/G-$cfg-4x1-$round.txt 2>&1 < /dev/null
    say "G $cfg 4x1 $round: $(grep block $O/G-$cfg-4x1-$round.txt | awk '{print $2":slow="$8",max_ms="$10}' | tr '\n' ' ')"
    timeout 900 env LD_PRELOAD=$L $cfg srun -K1 -N4 --ntasks-per-node=2 ~/f1probe 16384 > $O/G-$cfg-4x2-$round.txt 2>&1 < /dev/null
    say "G $cfg 4x2 $round: $(grep block $O/G-$cfg-4x2-$round.txt | awk '{print $2":slow="$8",max_ms="$10}' | tr '\n' ' ')"
  done
done
timeout 900 env LD_PRELOAD=$L OSOE_EXPERIMENT_GATHER_PIECE=16384 srun -K1 -N2 --ntasks-per-node=1 -c4 $B latency > $O/G-bench.txt 2>&1 < /dev/null
say "G bench with pieces: $(grep -E 'latency get .*bytes=(262144|1048576)' $O/G-bench.txt | sed -E 's/latency |target=1 where=remote |fastest_us=[0-9.]+//g' | tr -s ' ' | tr '\n' ';')"
say PHASE-G-DONE
