#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# F1 retest after pi-002's move to a new switch port and cable (2026-09-26,
# night): the trigger map at three shapes and two rank placements, every
# node's TCP timeouts read before and after each run. Outputs in ~/pi-results-f1-retest.
set -u
O=~/pi-results-f1-retest; mkdir -p $O; : > $O/summary.txt
say() { echo "$(date +%T) $*" | tee -a $O/summary.txt; }
timeouts() { srun -N4 --ntasks-per-node=1 sh -c 'echo "$(hostname)=$(nstat -az TcpExtTCPTimeouts | awk "NR>1{print \$2}")"' 2>/dev/null | sort | tr '\n' ' '; }
probe() {  # label, srun shape, blocks...
  local label=$1 shape=$2; shift 2
  local before; before=$(timeouts)
  timeout 1800 srun -K1 $shape ~/f1probe "$@" > $O/$label.txt 2>&1 < /dev/null
  say "$label exit $?: $(grep block $O/$label.txt | awk '{print $2":slow="$8",max_ms="$10}' | tr '\n' ' ')"
  say "    timeouts before: $before"
  say "    timeouts after:  $(timeouts)"
}
for round in a b; do
  probe A-4x1-$round "-N4 --ntasks-per-node=1" 16384 20480 24576 28672 32768 36864 40960 49152
  probe A-4x2-$round "-N4 --ntasks-per-node=2" 8192 12288 16384 20480 24576
  probe A-4x4-$round "-N4 --ntasks-per-node=4" 4096 8192 12288 16384
done
printf '%s\n' pi-002 pi-001 pi-003 pi-004 > ~/f1-hosts
SLURM_HOSTFILE=~/f1-hosts probe P-rank0-on-pi-002 "-n4 --distribution=arbitrary" 28672 32768 28672 32768
say DONE
