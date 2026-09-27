#!/bin/bash
# pi-runs-phase4b.sh: phase 4's measurements of its verified tree (3d5bbe1)
# at 4 x 4 from ~/osoe-pacer-phase4b, release, after the first soak's DONE:
# the quick runs, five minutes each of waits and deprecated, the counted
# builds' coverage - every exercise through rma's 792-round cycle, a mix, the
# start_pes program - then an hour of mix. Each node's retransmission
# timeouts before and after every run; outputs in ~/pacer-results-phase4b.
set -u
until grep -q DONE ~/pacer-results-phase4a/summary.txt 2>/dev/null; do sleep 30; done
cd ~/osoe-pacer-phase4b || exit 1
O=~/pacer-results-phase4b
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
  say "$label exit $status in $(( $(date +%s) - start )) s: $(grep -E '^(osoe-pacer|start-pes) failures=' $O/$label.txt)"
  say "    timeouts before: $before"
  say "    timeouts after:  $(timeouts)"
}
: > $O/summary.txt
say "commit $(cat COMMIT)"
srun -N4 --ntasks-per-node=1 sh -c 'echo "$(hostname) up=$(cut -d. -f1 /proc/uptime)s governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor) $(/usr/sbin/ethtool -k eth0 | grep -E "^(tcp|generic)-segmentation-offload" | tr -d " " | tr "\n" " ")"' 2>&1 | sort > $O/nodes.txt
cat $O/nodes.txt >> $O/summary.txt
run quick $S $R --quick --fold-nodes
run quick-mix $S $R --quick --fold-nodes mix
run quick-deprecated $S $R --quick --rounds 368 deprecated
run start-pes $S build/release/tools/pacer/osoe-pacer-start-pes
run waits $S $R --seconds 300 waits
run deprecated $S $R --seconds 300 deprecated
run coverage $S $C --quick --rounds 792 --coverage $O/coverage.txt sort alltoall mailbox teams \
    atomics rma locks waits memory collectives threads print deprecated
run coverage-mix $S $C --seconds 120 --coverage $O/coverage-mix.txt mix
run coverage-start-pes $S build/release/tools/pacer/osoe-pacer-start-pes-counted \
    --coverage $O/coverage-start-pes.txt
run mix-1h $S $R --seconds 3600 --fold-nodes mix
say DONE
