#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# F1 phase H: the chunk between 16 and 64 KiB, and a larger buffer at 64 KiB.
set -u
O=~/pi-results-f1; say() { echo "$(date +%T) $*" | tee -a $O/summary.txt; }
B=~/shmem-over-ethernet/build/release/tools/bench/osoe-bench
for cfg in OSOE_AM_CHUNK=32768 OSOE_AM_CHUNK=24576 OSOE_AM_BUFFER=262144 OSOE_AM_CHUNK=32768; do
  timeout 900 env $cfg srun -K1 -N4 --ntasks-per-node=1 ~/f1probe 24576 28672 32768 36864 > $O/H-$cfg-4x1.txt 2>&1 < /dev/null
  say "H $cfg 4x1: $(grep block $O/H-$cfg-4x1.txt | awk '{print $2":slow="$8",max_ms="$10}' | tr '\n' ' ')"
  timeout 900 env $cfg srun -K1 -N4 --ntasks-per-node=2 ~/f1probe 16384 > $O/H-$cfg-4x2.txt 2>&1 < /dev/null
  say "H $cfg 4x2: $(grep block $O/H-$cfg-4x2.txt | awk '{print $2":slow="$8",max_ms="$10}' | tr '\n' ' ')"
done
for cfg in OSOE_AM_CHUNK=32768 OSOE_AM_CHUNK=24576; do
  timeout 900 env $cfg srun -K1 -N2 --ntasks-per-node=1 -c4 $B latency bandwidth > $O/H-$cfg-bench.txt 2>&1 < /dev/null
  say "H $cfg bench: $(grep -E 'latency get .*bytes=(32768|262144|1048576)|bandwidth put .*bytes=1048576' $O/H-$cfg-bench.txt | sed -E 's/latency |target=1 where=remote |fastest_us=[0-9.]+//g' | tr -s ' ' | tr '\n' ';')"
done
say PHASE-H-DONE
