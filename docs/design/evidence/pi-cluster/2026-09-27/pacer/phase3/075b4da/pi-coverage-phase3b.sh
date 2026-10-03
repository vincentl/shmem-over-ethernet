#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The counted build of 075b4da over rma's full cycle, 560 rounds, at 4 x 4.
cd ~/osoe-pacer-phase3 || exit 1
O=~/pacer-results-phase3b
timeouts() { srun -N4 --ntasks-per-node=1 sh -c 'echo "$(hostname)=$(nstat -az TcpExtTCPTimeouts | awk "NR>1{print \$2}")"' 2>/dev/null | sort | tr "\n" " "; }
before=$(timeouts); start=$(date +%s)
srun -K1 -N4 --ntasks-per-node=4 build/release/tools/pacer/osoe-pacer-counted --quick --rounds 560 \
    --coverage $O/coverage.txt sort alltoall mailbox teams atomics rma locks memory collectives threads print \
    > $O/coverage-run.txt 2>&1 < /dev/null
status=$?
echo "$(date +%T) coverage exit $status in $(( $(date +%s) - start )) s: $(grep "^osoe-pacer failures=" $O/coverage-run.txt) $(head -1 $O/coverage.txt)" >> $O/summary.txt
echo "    timeouts before: $before" >> $O/summary.txt
echo "    timeouts after:  $(timeouts)" >> $O/summary.txt
echo DONE >> $O/summary.txt
