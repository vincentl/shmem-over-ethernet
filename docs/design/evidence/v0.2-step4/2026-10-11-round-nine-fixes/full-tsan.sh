#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The whole ThreadSanitizer suite of the fixes' tree as built by quick-tsan.sh,
# at each helper count, -j4 --timeout 600 as the verification runs it; the
# gate's verdict is ctest's (tsan.reports), its output and the report files kept.
set -u
W=/tmp/r9f; S=<repo>/build/scratch/review-nine-fixes; L=$S/logs/full-tsan; mkdir -p $L
cd $W/src
for h in ${COUNTS:-0 2 3}; do
  exec 9>/tmp/osoe-vm.lock
  flock -w 43200 9 || { echo "no lock"; exit 1; }
  echo "h$h: lock taken $(date +%T)"
  OSOE_PROGRESS_HELPERS=$h ctest --test-dir build/tsan -j4 --timeout 600 --output-on-failure > $L/tsan-h$h.full 2>&1
  echo "h$h: ctest exit $? $(date +%T)"
  grep -E 'tests passed|\*\*\*Failed|\*\*\*Timeout|\*\*\*Exception|Not Run' $L/tsan-h$h.full | head -20
  python3 -I cmake/thread-sanitizer-gate.py judge build/tsan/thread-sanitizer-reports --ctest ctest --build build/tsan > $L/gate-h$h.txt 2>&1
  tail -3 $L/gate-h$h.txt
  rm -rf $L/reports-h$h; cp -r build/tsan/thread-sanitizer-reports $L/reports-h$h
  exec 9>&-
done
echo DONE $(date +%T)
