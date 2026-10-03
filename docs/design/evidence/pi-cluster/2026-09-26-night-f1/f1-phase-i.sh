#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# F1 phase I: with a 16 KiB chunk, does the stall move to other block sizes?
set -u
O=~/pi-results-f1; say() { echo "$(date +%T) $*" | tee -a $O/summary.txt; }
for round in a b; do
  timeout 1800 env OSOE_AM_CHUNK=16384 srun -K1 -N4 --ntasks-per-node=1 ~/f1probe 4096 6144 8192 10240 12288 14336 16384 20480 24576 28672 32768 36864 49152 > $O/I-4x1-$round.txt 2>&1 < /dev/null
  say "I chunk16k 4x1 $round: $(grep block $O/I-4x1-$round.txt | awk '{print $2":slow="$8",max_ms="$10}' | tr '\n' ' ')"
  timeout 1800 env OSOE_AM_CHUNK=16384 srun -K1 -N4 --ntasks-per-node=2 ~/f1probe 4096 6144 8192 12288 16384 24576 > $O/I-4x2-$round.txt 2>&1 < /dev/null
  say "I chunk16k 4x2 $round: $(grep block $O/I-4x2-$round.txt | awk '{print $2":slow="$8",max_ms="$10}' | tr '\n' ' ')"
done
say PHASE-I-DONE
