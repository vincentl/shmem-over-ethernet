#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# P9-1's fix on the Pi: the fixes' tree (7d1d868) built fresh under ~/osoe, then
# tier 2 on one Pi node as the runbook runs it - srun -N1 -n1 -c4, ctest -j4 -
# three times at the limits the step is given, which failed 4 runs of 4 at
# 3b45472 (758 or 759 of 760, refused rings), with the step's limits recorded.
set -u
P=<repo>/build/scratch/review-nine-fixes/pi
LOG=$P/run.txt; : > $LOG
PI="ssh -o ConnectTimeout=20 -o ServerAliveInterval=30 pi-login"
echo "start $(date +%H:%M:%S)" >> $LOG
rsync -a --delete --exclude=/build --exclude=/.git $P/tree/ pi-login:osoe/ || { echo "rsync failed" >> $LOG; exit 1; }
$PI 'cd osoe && rm -rf build/release build/release-run && mkdir -p build/release-run' || exit 1
$PI 'cd osoe && (cmake --preset release && cmake --build build/release -j4) > build/release-run/build.txt 2>&1; echo "build: $?"; grep -c "warning:" build/release-run/build.txt | sed "s/^/warnings: /"' >> $LOG
$PI 'srun -N1 -n1 -c4 sh -c "echo step limits: soft \$(ulimit -Sl) hard \$(ulimit -Hl); uname -r"' >> $LOG 2>&1
for i in 1 2 3; do
  $PI "cd osoe && srun -N1 -n1 -c4 ctest --test-dir build/release -L tier2 -E '^model\.' -j4 --timeout 300 > build/release-run/tier2-$i.txt 2>&1; echo \"tier2 run $i: \$? \$(grep 'tests passed' build/release-run/tier2-$i.txt)\"; grep -E '\*\*\*Failed|Timeout|Exception' build/release-run/tier2-$i.txt | head -8" >> $LOG 2>&1
done
scp -q "pi-login:osoe/build/release-run/*.txt" $P/ 2>> $LOG
echo "DONE $(date +%H:%M:%S)" >> $LOG
