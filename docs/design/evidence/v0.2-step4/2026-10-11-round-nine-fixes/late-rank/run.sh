#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# A node-mate that arrives after its lead has died: does it end by itself,
# naming the lead, or wait for ever? The debug build of the fixes' tree
# (quick-debug.sh's), rank 1 started 1, 2, 4 and 6 s in, the lead killed at 3 s,
# the launcher's limit 30 s; then main's tree the same way, for comparison.
set -u
S=<repo>/build/scratch/review-nine-fixes
exec 9>/tmp/osoe-vm.lock; flock -w 43200 9 || exit 1
for tree in /tmp/r9f-debug/src; do
  for d in 1 2 4 6; do
    s=$(date +%s%N)
    OSOE_TEST_NOTICE_SECONDS=2 timeout 60 $tree/build/debug/tools/launch/osoe-launch -k -n 3 -t 30 -- \
      sh $S/late-rank/lead-ends-late.sh 3 2 1 $d $tree/build/debug/shmem/tests/shmem_test_init > $S/late-rank/delay-$d.txt 2>&1
    e=$(date +%s%N)
    echo "rank 1 at ${d} s: $(( (e - s) / 1000000 )) ms; $(grep -c 'has ended while this PE waited' $S/late-rank/delay-$d.txt) lead-ended lines; $(grep -c 'timeout after' $S/late-rank/delay-$d.txt) timeouts"
    grep -E 'osoe: PE 1|rank 1' $S/late-rank/delay-$d.txt | head -4
  done
done
