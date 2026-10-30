#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# x86-tag.sh's full build, its one-node tests again. Its first run stalled Rosetta's translation
# daemon - rosettad at 360% of the CPU with 515 threads, the load at 325 - after translating the
# suite's new binaries at once, every start then timing out by the hundred, as round nine's first
# run did (docs/reviews/archive/v0.2-before-part4-portability.md, "Under Rosetta"). The VM was
# restarted, the daemon with it, and the same build's tests run again here, two at a time, with
# the shim copied into the VM's own file system; then the failures again, one at a time.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
W=/var/tmp/x86-tag SUMMARY=$HERE/x86-tag.summary SHIM=/tmp/x86-tag-shim
say() { echo "$*" | tee -a "$SUMMARY"; }
[ -d $W/full/build/release ] || { say "the full build is gone"; exit 2; }
[ -f $W/full-ctest.log ] && mv $W/full-ctest.log $W/full-ctest-stalled.log
say "-- $(date -u +%T) the full build's first run stalled Rosetta's daemon ($(grep -c -E 'Failed|Timeout' $W/full-ctest-stalled.log) failed or timed out by test 429 of 581); the VM restarted; again, load $(cut -d' ' -f1-3 /proc/loadavg)"
mkdir -p $SHIM; [ -d $SHIM/lib ] || cp -r <repo>/build/scratch/review-r3/x86/shim/lib $SHIM/
export LD_PRELOAD="$SHIM/\$LIB/libfutex2-shim.so"
cd $W/full/build/release
( time timeout 3000 ctest -j2 -LE 'internode|model' --timeout 300 ) > $W/full-ctest.log 2>&1
say "full, one-node tests under Rosetta with the shim: $(grep -E 'tests passed' $W/full-ctest.log); $(grep -E '^real' $W/full-ctest.log); $(grep -c 'cannot be preloaded' $W/full-ctest.log) preload refusals"
grep -E '^\s+[0-9]+ - ' $W/full-ctest.log | head -20 | tee -a "$SUMMARY"
timeout 1500 ctest --rerun-failed -j1 --timeout 300 > $W/full-rerun.log 2>&1
say "full's failures again, one at a time: $(grep -E 'tests passed' $W/full-rerun.log)"
grep -E '^\s+[0-9]+ - ' $W/full-rerun.log | head -20 | tee -a "$SUMMARY"
mkdir -p $HERE/x86-tag && cp $W/*.log $HERE/x86-tag/
say "== $(date -u +%T) done"
