#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# pi-runs-phase3.sh: the pacer's measurements of phase 3
# (docs/prompts/task-stress-phase3.md) at 4 x 4 from ~/osoe-pacer-phase3: the
# quick run of every exercise, ten minutes each of atomics, rma, locks,
# memory and collectives in release, and the counted build's coverage of one
# default run. Outputs in ~/pacer-results-phase3; each node's retransmission
# timeouts before and after every run. Waits for any earlier script's DONE.
set -u
until { grep -q CPU-BIND-DONE ~/pacer-results-phase2b/summary.txt 2>/dev/null; }; do sleep 30; done
until grep -q BUILD-EXIT ~/build-phase3.log 2>/dev/null; do sleep 10; done
grep -q 'BUILD-EXIT 0' ~/build-phase3.log || { echo "no build" > ~/pacer-results-phase3.failed; exit 1; }
cd ~/osoe-pacer-phase3 || exit 1
O=~/pacer-results-phase3
mkdir -p $O
R=build/release/tools/pacer/osoe-pacer
C=build/release/tools/pacer/osoe-pacer-counted
S="srun -K1 -N4 --ntasks-per-node=4"
say() { echo "$(date +%T) $*" | tee -a $O/summary.txt; }
timeouts() {
  srun -N4 --ntasks-per-node=1 sh -c 'echo "$(hostname)=$(nstat -az TcpExtTCPTimeouts | awk "NR>1{print \$2}")"' 2>/dev/null | sort | tr '\n' ' '
}
run() {                             # label, then the command's arguments
  local label=$1; shift
  local before; before=$(timeouts)
  local start; start=$(date +%s)
  "$@" > $O/$label.txt 2>&1 < /dev/null
  local status=$?
  say "$label exit $status in $(( $(date +%s) - start )) s: $(grep '^osoe-pacer failures=' $O/$label.txt)"
  say "    timeouts before: $before"
  say "    timeouts after:  $(timeouts)"
}
: > $O/summary.txt
say "commit $(cat COMMIT)"
srun -N4 --ntasks-per-node=1 sh -c 'echo "$(hostname) up=$(cut -d. -f1 /proc/uptime)s governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor) $(/usr/sbin/ethtool -k eth0 | grep -E "^(tcp|generic)-segmentation-offload" | tr -d " " | tr "\n" " ")"' 2>&1 | sort > $O/nodes.txt
cat $O/nodes.txt >> $O/summary.txt
run quick $S $R --quick
for e in atomics rma locks memory collectives; do
  run $e $S $R --seconds 600 $e
done
run coverage $S $C --quick --rounds 426 --coverage $O/coverage.txt sort alltoall mailbox teams atomics \
    rma locks memory collectives threads print
say DONE
