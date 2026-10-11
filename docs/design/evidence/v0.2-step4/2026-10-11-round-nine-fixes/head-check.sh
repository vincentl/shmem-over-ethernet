#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The branch head's changes since the verified tree (234b622): the static checks
# and echo-uring's tests (its client now looks up IPv4), in the release preset.
set -u
S=<repo>/build/scratch/review-nine-fixes; L=$S/logs; mkdir -p $L
exec 9>/tmp/osoe-vm.lock; flock -w 43200 9 || exit 1
W=/tmp/r9f-head; rm -rf $W; mkdir -p $W; tar -xf $S/head.tar -C $W; cd $W
cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > $L/head-build.txt 2>&1 || { echo BUILD FAILED; exit 1; }
echo "warnings: $(grep -c 'warning:' $L/head-build.txt)"
ctest --test-dir build/release -j4 --output-on-failure \
  -R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|license-lines|single-node\.configure|version\.)|echo-uring|floor' \
  > $L/head-check.txt 2>&1
grep -E 'tests passed|Failed|Test +#' $L/head-check.txt | tail -25
cd /; rm -rf $W
