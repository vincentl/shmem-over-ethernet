#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Tier 2 on one Pi node, three times, as the runbook runs it, the fixes' tree
# with the many-PE jobs run alone (RUN_SERIAL) and the lead-ends test's 30 s:
# rsynced and built fresh in ~/osoe, every run inside one ssh session with a
# second held open throughout, the step's limits recorded.
set -u
P=<repo>/build/scratch/review-nine-fixes/pi
LOG=$P/serial.txt; : > $LOG
ssh -o ConnectTimeout=20 -o ServerAliveInterval=30 pi-login 'sleep 7200' &
HOLD=$!
sleep 5
rsync -a --delete --exclude=/build --exclude=/.git $P/tree/ pi-login:osoe/ || { echo "rsync failed" >> $LOG; exit 1; }
ssh -o ConnectTimeout=20 -o ServerAliveInterval=30 pi-login "cd osoe && rm -rf build/release && mkdir -p build/release-run && (cmake --preset release && cmake --build build/release -j4) > build/release-run/build.txt 2>&1; echo \"build: \$?, warnings \$(grep -c warning: build/release-run/build.txt)\"
  srun -N1 -n1 -c4 sh -c 'echo step limits: soft \$(ulimit -Sl) hard \$(ulimit -Hl)'
  for i in 1 2 3; do
    srun -N1 -n1 -c4 ctest --test-dir build/release -L tier2 -E '^model\.' -j4 --timeout 300 --output-on-failure > build/release-run/serial-\$i.txt 2>&1
    echo \"serial tier2 run \$i: \$? \$(grep 'tests passed' build/release-run/serial-\$i.txt) \$(grep 'Total Test time' build/release-run/serial-\$i.txt)\"; grep -E '[*][*][*]Failed|Timeout' build/release-run/serial-\$i.txt | head -5
  done" >> $LOG 2>&1
scp -q "pi-login:osoe/build/release-run/serial-*.txt" $P/ 2>> $LOG
kill $HOLD
echo "DONE $(date +%H:%M:%S)" >> $LOG
