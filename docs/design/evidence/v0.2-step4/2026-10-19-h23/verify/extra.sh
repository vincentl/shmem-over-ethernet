#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# H23 beyond the batch: the internode tests at two progress helpers, and the
# mesh's one-line notice seen, node 1 of three held 4 s, notices after 1 s.
cd /tmp/h23-verify || exit 1
OSOE_PROGRESS_HELPERS=2 ctest --test-dir build/release -L internode -LE "^model$" -j4 --timeout 600 > /tmp/h23-extra-h2.txt 2>&1
echo "internode at 2 helpers: $(grep -E "tests passed" /tmp/h23-extra-h2.txt)"; grep -E "\(Failed\)|\(Timeout\)" /tmp/h23-extra-h2.txt | head -8
OSOE_TEST_NOTICE_SECONDS=1 OSOE_TEST_MESH_HOLD=1:4000 build/release/tools/launch/osoe-launch -n 3 -m 0,1,2 -t 30 -- build/release/shmem/tests/shmem_test_barrier > /tmp/h23-extra-notice.txt 2>&1
echo "held job: status $?"; grep "has waited" /tmp/h23-extra-notice.txt | head -6
echo "EXTRA DONE $(date +%H:%M:%S)"
