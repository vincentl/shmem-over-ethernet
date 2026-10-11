#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Whether shmem.start-up.lead-ends-in-join's failures on the Pi are the fixes':
# its job run directly, as refusal.sh runs it, twenty times on one node under
# srun, on main (3b45472) and on the fixes' tree (7d1d868), each built fresh in
# ~/osoe; each run's output kept, with its time and whether the pattern came.
set -u
P=<repo>/build/scratch/review-nine-fixes/pi
LOG=$P/ab.txt; : > $LOG
PI="ssh -o ConnectTimeout=20 -o ServerAliveInterval=30 pi-login"
for tree in main-tree tree; do
  rsync -a --delete --exclude=/build --exclude=/.git $P/$tree/ pi-login:osoe/ || { echo "rsync failed" >> $LOG; exit 1; }
  $PI 'cd osoe && rm -rf build/release && (cmake --preset release && cmake --build build/release -j4) > /dev/null 2>&1; echo "build: $?"' >> $LOG
  $PI "cd osoe && mkdir -p build/ab && srun -N1 -n1 -c4 sh -c '
    for i in \$(seq 1 20); do
      s=\$(date +%s%N)
      OSOE_TEST_NOTICE_SECONDS=2 timeout 40 build/release/tools/launch/osoe-launch -k -n 3 -t 8 -- \
        sh shmem/tests/lead-ends.sh 3 2 build/release/shmem/tests/shmem_test_init > build/ab/$tree-\$i.txt 2>&1
      st=\$?
      e=\$(date +%s%N)
      ok=\$(grep -c \"has ended while this PE waited for local PEs to join the node\" build/ab/$tree-\$i.txt)
      echo \"$tree run \$i: status \$st, \$(( (\$e - \$s) / 1000000 )) ms, pattern \$ok, timeout lines \$(grep -c \"timeout after\" build/ab/$tree-\$i.txt)\"
    done'" >> $LOG 2>&1
  mkdir -p $P/ab-$tree; scp -q "pi-login:osoe/build/ab/$tree-*.txt" $P/ab-$tree/ 2>> $LOG
done
echo "DONE $(date +%H:%M:%S)" >> $LOG
