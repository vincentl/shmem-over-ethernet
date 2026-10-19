#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
cd /tmp/h23-before || exit 1
cmake --preset tier3 > /tmp/h23-before-cfg.txt 2>&1 && cmake --build build/tier3 -j4 > /tmp/h23-before-build.txt 2>&1 || { echo BUILD FAILED; exit 1; }
echo "built, warnings $(grep -c warning: /tmp/h23-before-build.txt)"
# the guard: the default table refuses 64 nodes by name
sudo -n tools/launch/tier3.sh pi 64 -- /bin/true; echo "tier3.sh at 64 with the default table: status $?"
sudo -n tools/launch/tier3.sh pi 32 -- build/tier3/tools/launch/osoe-launch -n 32 -m $(seq -s, 0 31) -t 120 -- build/tier3/tools/bench/osoe-bench barrier quick > /dev/null 2>&1; echo "tier3.sh at 32 with the default table: status $?"
/tmp/h23-measure.sh /tmp/h23-before before 3 "tier3" 16 32 64 128
/tmp/h23-measure.sh /tmp/h23-before before 3 "loopback" 16 32 64 128 254
echo "BEFORE DONE $(date +%H:%M:%S)"
