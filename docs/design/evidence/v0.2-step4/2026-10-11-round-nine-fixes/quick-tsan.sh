#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The ThreadSanitizer build of the fixes' tree, and the tests the fixes touch,
# at two helpers, under the VM's lock: the gate's fixture, the leaks' four
# tests, the new and widened tests.
set -u
W=/tmp/r9f; S=<repo>/build/scratch/review-nine-fixes; L=$S/logs; mkdir -p $L
exec 9>/tmp/osoe-vm.lock
flock -w 43200 9 || { echo "no lock"; exit 1; }
echo "lock taken $(date +%T)"
rm -rf $W; mkdir -p $W/src; tar -xf $S/tree.tar -C $W/src
cd $W/src
cmake --preset tsan > $L/tsan-configure.txt 2>&1 || { echo CONFIGURE FAILED; tail -20 $L/tsan-configure.txt; exit 1; }
cmake --build --preset tsan -j4 > $L/tsan-build.txt 2>&1 || { echo BUILD FAILED; grep -E 'error|warning' $L/tsan-build.txt | head; exit 1; }
echo "built $(date +%T)"
ctest --test-dir build/tsan -N | tail -2
OSOE_PROGRESS_HELPERS=2 ctest --test-dir build/tsan -j4 --timeout 600 --output-on-failure \
  -R 'shmem[.]limits|get-no-progress|helpers[.]deaf|kill-after-init[.]TERM|dead-pe[.]lead[.]0-0-1-1|reply-pool[.]overflow' \
  > $L/tsan-quick.txt 2>&1
echo "ctest exit $? $(date +%T)"
grep -E 'tests passed|Test +#|THREAD SANITIZER|report files|passed:' $L/tsan-quick.txt | tail -30
