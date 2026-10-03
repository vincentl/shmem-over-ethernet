#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# pi-runs-phase2.sh: the pacer's measurements of phase 2
# (docs/prompts/task-stress-phase2.md) at 4 x 4 from ~/osoe-pacer: mailbox and
# teams ten minutes each in release, then an hour of threads in a debug build
# with both of the library's test hooks on and a landing zone its gets
# overfill. Outputs in ~/pacer-results-phase2; each node's retransmission
# timeouts before and after every run. Run it under nohup.
set -u
cd ~/osoe-pacer || exit 1
O=~/pacer-results-phase2
mkdir -p $O
R=build/release/tools/pacer/osoe-pacer
D=build/debug/tools/pacer/osoe-pacer
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
  say "$label exit $status in $(( $(date +%s) - start )) s: $(grep '^osoe-pacer failures=' $O/$label.txt) $(grep -m1 -o 'OSOE_CHECK_QUIET:.*' $O/$label.txt)"
  say "    timeouts before: $before"
  say "    timeouts after:  $(timeouts)"
}
: > $O/summary.txt
say "commit $(cat COMMIT)"
srun -N4 --ntasks-per-node=1 sh -c 'echo "$(hostname) up=$(cut -d. -f1 /proc/uptime)s governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor) $(/usr/sbin/ethtool -k eth0 | grep -E "^(tcp|generic)-segmentation-offload" | tr -d " " | tr "\n" " ")"' 2>&1 | sort > $O/nodes.txt
cat $O/nodes.txt >> $O/summary.txt
run quick $S $R --quick sort alltoall mailbox teams
run mailbox $S $R --seconds 600 mailbox
run teams $S $R --seconds 600 teams
run threads-hour env OSOE_CHECK_QUIET=1 OSOE_TEST_PERTURB=1 OSOE_GET_LANDING_BYTES=8192 \
    $S $D --seconds 3600 threads
say DONE
