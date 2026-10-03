#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# pi-runs-phase2b.sh: phase 2's tip after the round trip was added (01d003f,
# ~/osoe-pacer-01d003f): once pi-runs-phase2.sh has finished, the quick run and
# mailbox's ten minutes again, in release at 4 x 4. Outputs in
# ~/pacer-results-phase2b. Run it under nohup.
set -u
until grep -q DONE ~/pacer-results-phase2/summary.txt 2>/dev/null; do sleep 30; done
cd ~/osoe-pacer-01d003f || exit 1
grep -q 'BUILD-EXIT 0' ~/build-01d003f.log || { echo "no build" > ~/pacer-results-phase2b.failed; exit 1; }
O=~/pacer-results-phase2b
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
run quick $S $R --quick sort alltoall mailbox teams
run mailbox $S $R --seconds 600 mailbox
say DONE
