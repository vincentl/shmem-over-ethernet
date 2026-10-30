#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# x86-tag.sh's full build, its one-node tests a third time. Twice, two at a time, they stalled
# Rosetta's translation daemon (rosettad at 360-390% of the CPU with 515-525 threads, the load
# above 320) - the second time on a VM restarted for it - every start then timing out by the
# hundred. Here one test at a time, so that the daemon never translates the suite's new binaries
# together, on the VM restarted again; the shim copied into the VM's own file system as before.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
W=/var/tmp/x86-tag SUMMARY=$HERE/x86-tag.summary SHIM=/tmp/x86-tag-shim
say() { echo "$*" | tee -a "$SUMMARY"; }
[ -d $W/full/build/release ] || { say "the full build is gone"; exit 2; }
[ -f $W/full-ctest.log ] && mv $W/full-ctest.log $W/full-ctest-stalled-2.log
say "-- $(date -u +%T) the second run stalled the daemon too ($(grep -c -E 'Failed|Timeout' $W/full-ctest-stalled-2.log) failed or timed out by test 461 of 581); the VM restarted; one at a time, load $(cut -d' ' -f1-3 /proc/loadavg)"
mkdir -p $SHIM; [ -d $SHIM/lib ] || cp -r <repo>/build/scratch/review-r3/x86/shim/lib $SHIM/
export LD_PRELOAD="$SHIM/\$LIB/libfutex2-shim.so"
cd $W/full/build/release
( time timeout 2700 ctest -j1 -LE 'internode|model' --timeout 300 ) > $W/full-ctest.log 2>&1
say "full, one-node tests under Rosetta with the shim, one at a time: $(grep -E 'tests passed' $W/full-ctest.log); $(grep -E '^real' $W/full-ctest.log); $(grep -c 'cannot be preloaded' $W/full-ctest.log) preload refusals; the daemon at the end: $(ps -eo pcpu,nlwp,comm | awk '/rosettad/ { print $1 "% CPU, " $2 " threads" }')"
grep -E '^\s+[0-9]+ - ' $W/full-ctest.log | head -24 | tee -a "$SUMMARY"
mkdir -p $HERE/x86-tag && cp $W/*.log $HERE/x86-tag/
say "== $(date -u +%T) done"
