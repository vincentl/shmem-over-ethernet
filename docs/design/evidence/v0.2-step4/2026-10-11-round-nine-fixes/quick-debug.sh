#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The debug build of the fixes' tree: the static checks, then the tests the
# fixes touch, at no helpers and at two. Under the VM's lock.
set -u
W=/tmp/r9f-debug; S=<repo>/build/scratch/review-nine-fixes; L=$S/logs; mkdir -p $L
exec 9>/tmp/osoe-vm.lock
flock -w 43200 9 || { echo "no lock"; exit 1; }
rm -rf $W; mkdir -p $W/src; tar -xf $S/tree.tar -C $W/src 2>/dev/null; cd $W/src
cmake --preset debug > $L/debug-configure.txt 2>&1 || { echo CONFIGURE FAILED; tail $L/debug-configure.txt; exit 1; }
cmake --build --preset debug -j4 > $L/debug-build.txt 2>&1 || { echo BUILD FAILED; grep -E 'error|warning' $L/debug-build.txt | head; exit 1; }
grep -c 'warning' $L/debug-build.txt
ctest --test-dir build/debug -j4 -L tier1 > $L/debug-tier1.txt 2>&1; echo "tier1: $(grep -E 'tests passed' $L/debug-tier1.txt)"; grep -E '\*\*\*Failed|Failed  |Timeout' $L/debug-tier1.txt | head
for h in 0 2; do
  OSOE_PROGRESS_HELPERS=$h ctest --test-dir build/debug -j4 --output-on-failure \
    -R 'shmem[.]limits|get-no-progress|stopped-node|stopped-bulk|helpers[.]|kill-after-init|dead-pe|reply-pool|multiple|blocking|get-pieces|circulant[.]4$|abort|global-exit' \
    > $L/debug-touched-h$h.txt 2>&1
  echo "h$h: $(grep -E 'tests passed' $L/debug-touched-h$h.txt)"; grep -E '\*\*\*Failed|\(Failed\)|Timeout' $L/debug-touched-h$h.txt | head
done
