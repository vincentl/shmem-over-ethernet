#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The two tests that failed in tier 2's runs 2 and 3 on the Pi with the fixes'
# tree: alone five times, then three times beside the large jobs that the
# locked-memory raise now lets run (before, refused at start-up in a second),
# each under srun on one node as the runbook runs tier 2, with their output.
set -u
P=<repo>/build/scratch/review-nine-fixes/pi
LOG=$P/flaky.txt; : > $LOG
PI="ssh -o ConnectTimeout=20 -o ServerAliveInterval=30 pi-login"
T='shmem[.]leftovers[.]restart[.]other-size$|shmem[.]start-up[.]lead-ends-in-join$'
L='|shmem[.]circulant[.]70$|shmem[.]circulant[.]64$|shmem[.]shape[.]chosen[.]pi[.]64$|shmem[.]shape[.]chosen[.]loopback[.]64$'
for i in 1 2 3 4 5; do
  $PI "cd osoe && srun -N1 -n1 -c4 ctest --test-dir build/release -R '$T' -j2 --timeout 300 --output-on-failure > build/release-run/alone-$i.txt 2>&1; echo \"alone $i: \$(grep 'tests passed' build/release-run/alone-$i.txt)\"" >> $LOG 2>&1
done
for i in 1 2 3; do
  $PI "cd osoe && srun -N1 -n1 -c4 ctest --test-dir build/release -R '$T$L' -j4 --timeout 300 --output-on-failure > build/release-run/loaded-$i.txt 2>&1; echo \"loaded $i: \$(grep 'tests passed' build/release-run/loaded-$i.txt)\"" >> $LOG 2>&1
done
scp -q "pi-login:osoe/build/release-run/alone-*.txt" "pi-login:osoe/build/release-run/loaded-*.txt" $P/ 2>> $LOG
echo "DONE $(date +%H:%M:%S)" >> $LOG
