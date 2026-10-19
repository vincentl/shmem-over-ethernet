#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# H23: the order before and the schedule after, alternating round by round.
cd /tmp/h23-wip || exit 1
cmake --preset tier3 > /tmp/h23-after-cfg.txt 2>&1 && cmake --build build/tier3 -j4 > /tmp/h23-after-build.txt 2>&1 || { echo BUILD FAILED; exit 1; }
echo "built after, warnings $(grep -c warning: /tmp/h23-after-build.txt)"
for n in 16 32 64 128; do
  for r in 1 2 3; do
    /tmp/h23-measure.sh /tmp/h23-before before-r$r 1 "tier3 loopback" $n
    /tmp/h23-measure.sh /tmp/h23-wip after-r$r 1 "tier3 loopback" $n
  done
done
for r in 1 2 3; do
  /tmp/h23-measure.sh /tmp/h23-before before-r$r 1 loopback 254
  /tmp/h23-measure.sh /tmp/h23-wip after-r$r 1 loopback 254
done
echo "ALTERNATE DONE $(date +%H:%M:%S)"
