#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# pi-runs.sh: the pacer's measurements of phase 1 (docs/pi-cluster.md §6) at
# 4 x 4, release, from ~/osoe-pacer; outputs in ~/pacer-results. Each node's
# retransmission timeouts are read before and after every run, as F1's retests
# did, and the offload state once. Run it under nohup: a dropped ssh must not
# end a ten-minute run.
set -u
cd ~/osoe-pacer || exit 1
O=~/pacer-results
mkdir -p $O
P=build/release/tools/pacer/osoe-pacer
B=build/release/tools/bench/osoe-bench
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
run quick $S $P --quick
run default $S $P --seconds 300
run accumulate env SHMEM_SYMMETRIC_SIZE=1500m $S $P --seconds 600 --sort accumulate --memory 1g sort
run pull $S $P --seconds 600 --exchange alltoall sort
run no-ptr $S $P --seconds 600 --no-ptr sort
run bench-collectives $S $B collectives
say DONE
