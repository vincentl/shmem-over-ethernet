#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Tier 2 on one Pi node, three times, as the runbook runs it, the fixes' tree
# (already built in ~/osoe by ab.sh), every run inside one ssh session, and a
# second session held open from before the first run until after the last: no
# session ends while a job runs, so RemoveIPC cannot remove the user's
# /dev/shm objects under them (docs/pi-cluster.md).
set -u
P=<repo>/build/scratch/review-nine-fixes/pi
LOG=$P/held.txt; : > $LOG
ssh -o ConnectTimeout=20 -o ServerAliveInterval=30 pi-login 'sleep 7200' &
HOLD=$!
sleep 5
ssh -o ConnectTimeout=20 -o ServerAliveInterval=30 pi-login "cd osoe && for i in 1 2 3; do
  srun -N1 -n1 -c4 ctest --test-dir build/release -L tier2 -E '^model\.' -j4 --timeout 300 --output-on-failure > build/release-run/held-\$i.txt 2>&1
  echo \"held tier2 run \$i: \$? \$(grep 'tests passed' build/release-run/held-\$i.txt)\"; grep -E '[*][*][*]Failed|Timeout' build/release-run/held-\$i.txt | head -8
done" >> $LOG 2>&1
scp -q "pi-login:osoe/build/release-run/held-*.txt" $P/ 2>> $LOG
kill $HOLD
echo "DONE $(date +%H:%M:%S)" >> $LOG
