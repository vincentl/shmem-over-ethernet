#!/bin/bash
# pi-soak-phase4a.sh: phase 4's first soak (docs/prompts/task-stress-phase4.md,
# "The measurement") at 4 x 4 from ~/osoe-pacer-phase4, release: the quick mix,
# then three hours of mix with labels folded to nodes. Outputs in
# ~/pacer-results-phase4a; each node's retransmission timeouts before and after.
set -u
cd ~/osoe-pacer-phase4 || exit 1
O=~/pacer-results-phase4a
mkdir -p $O
R=build/release/tools/pacer/osoe-pacer
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
run quick-mix $S $R --quick --fold-nodes mix
run mix-3h $S $R --seconds 10800 --fold-nodes mix
say DONE
