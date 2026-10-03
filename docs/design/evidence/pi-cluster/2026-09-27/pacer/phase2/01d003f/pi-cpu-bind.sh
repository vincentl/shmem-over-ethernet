#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# pi-cpu-bind.sh: does mailbox's tail between node-mates come from each PE's
# threads sharing one core? Slurm binds each of a node's four tasks to one
# core, which a PE's main thread and its agent share (runbook §6). Once
# pi-runs-phase2b.sh is done: two minutes of mailbox unbound, then two with
# the default binding, release at 01d003f, 4 x 4. Outputs beside phase2b's.
set -u
until grep -q DONE ~/pacer-results-phase2b/summary.txt 2>/dev/null; do sleep 30; done
cd ~/osoe-pacer-01d003f || exit 1
O=~/pacer-results-phase2b
R=build/release/tools/pacer/osoe-pacer
say() { echo "$(date +%T) $*" | tee -a $O/summary.txt; }
start=$(date +%s)
srun -K1 -N4 --ntasks-per-node=4 --cpu-bind=none $R --seconds 120 mailbox > $O/mailbox-unbound.txt 2>&1 < /dev/null
say "mailbox-unbound exit $? in $(( $(date +%s) - start )) s: $(grep '^osoe-pacer failures=' $O/mailbox-unbound.txt)"
start=$(date +%s)
srun -K1 -N4 --ntasks-per-node=4 $R --seconds 120 mailbox > $O/mailbox-bound.txt 2>&1 < /dev/null
say "mailbox-bound exit $? in $(( $(date +%s) - start )) s: $(grep '^osoe-pacer failures=' $O/mailbox-bound.txt)"
say CPU-BIND-DONE
