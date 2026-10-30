#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The checklist's x86-64 item on the tree to be tagged, as 0.1.0's and round nine's
# (build/scratch/review-nine/portability/scripts/x-build.sh, x-run.sh, x-run2.sh), in the
# osoe-x86 VM: the tree cross-built for x86-64 with GCC 14.2, the single-node build and the full
# build, release preset, each without a warning; the single-node build's suite less the models
# under Rosetta, four at a time; the full build's one-node tests (not internode, not the models)
# under Rosetta with round three's futex2 shim, copied into the VM's own file system (read from the
# host's mount, Rosetta's daemon stalled a suite: portability Q9), two at a time; then each
# build's failures again, one at a time. On the VM's disk; the tree removed after.
#   x86-tag.sh TAR
set -u
TAR=$1
HERE=$(cd "$(dirname "$0")" && pwd)
W=/var/tmp/x86-tag SUMMARY=$HERE/x86-tag.summary SHIM=/tmp/x86-tag-shim
: > "$SUMMARY"
say() { echo "$*" | tee -a "$SUMMARY"; }
rm -rf $W; mkdir -p $W/sn $W/full
(cd $W/sn && tar -xf "$TAR") && (cd $W/full && tar -xf "$TAR") || { say "no tree"; exit 2; }
say "== $(date -u +%FT%TZ) x86-tag on $(. /etc/os-release; echo $PRETTY_NAME), $(uname -r), $(x86_64-linux-gnu-gcc --version | head -1)"
cd $W/sn
CC=x86_64-linux-gnu-gcc cmake --preset release -DOSOE_SINGLE_NODE=ON > $W/sn-configure.log 2>&1 &&
    cmake --build --preset release -j4 > $W/sn-build.log 2>&1 || say "single-node: did not configure or build"
say "single-node built: $(grep -ciE 'warning:' $W/sn-build.log) warnings; $(find build/release -name 'libosoe.so.0.*' -exec file -b {} \; | cut -d, -f1-2)"
cd $W/full
PKG_CONFIG_LIBDIR=/usr/lib/x86_64-linux-gnu/pkgconfig CC=x86_64-linux-gnu-gcc cmake --preset release > $W/full-configure.log 2>&1 &&
    cmake --build --preset release -j4 > $W/full-build.log 2>&1 || say "full: did not configure or build"
say "full built: $(grep -ciE 'warning:' $W/full-build.log) warnings; $(grep -m1 -i 'liburing' $W/full-configure.log | sed 's/^-- //')"
cd $W/sn/build/release
( time timeout 1100 ctest -j4 -LE model --timeout 300 ) > $W/sn-ctest.log 2>&1
say "single-node under Rosetta: $(grep -E 'tests passed' $W/sn-ctest.log); $(grep -E '^real' $W/sn-ctest.log)"
grep -E '^\s+[0-9]+ - ' $W/sn-ctest.log | head -20 | tee -a "$SUMMARY"
timeout 900 ctest --rerun-failed -j1 --timeout 300 > $W/sn-rerun.log 2>&1
say "single-node's failures again, one at a time: $(grep -E 'tests passed' $W/sn-rerun.log)"
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
