#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The zero-copy refusal on the Pi (Linux 6.18, 16 KiB pages), where io_uring
# charges its rings to the locked-memory limit and the first design of the test,
# `ulimit -l 64` before the job, would have failed at ring creation (the read of
# the change, finding 2): the branch built in ~/osoe/build/zero-copy-src; its two
# refused tests and the zero-copy ones on one node; then a job of two logical
# nodes under `ulimit -l 0`, whose start-up check must name the limit.
# usage: pi-zero-copy-refused.sh BRANCH_TAR OUTDIR (from the Mac).
set -u
BRANCH=$1 OUT=$2
mkdir -p "$OUT/tree"; LOG=$OUT/run.txt; : > "$LOG"
tar -C "$OUT/tree" -xf "$BRANCH"
echo "start $(date +%H:%M:%S)" >> "$LOG"
ssh pi-login 'rm -rf osoe/build/zero-copy-src && mkdir -p osoe/build/zero-copy-src' || exit 1
rsync -a --exclude=/build --exclude=/.git "$OUT/tree"/ pi-login:osoe/build/zero-copy-src/ || exit 1
ssh pi-login 'cd osoe/build/zero-copy-src && cmake --preset release > /dev/null 2>&1 &&
  cmake --build build/release -j4 > build-release.txt 2>&1; echo "built: $?, warnings $(grep -c warning: build-release.txt)"' >> "$LOG"
ssh pi-login "cd osoe/build/zero-copy-src && srun -N1 -n1 -c4 ctest --test-dir build/release -V --timeout 300 -R '^shmem\.(wire\.zero-copy|internode\.zero-copy)'" \
  > "$OUT/tests.txt" 2>&1
grep -E "tests passed|Test +#.*(Passed|Failed|Timeout|Skipped)|zero copy: |refused for want" "$OUT/tests.txt" \
  | sed 's/^[0-9]*: //' >> "$LOG"
echo "== two logical nodes under ulimit -l 0" >> "$LOG"
ssh pi-login "cd osoe/build/zero-copy-src && srun -N1 -n1 -c4 bash -c 'ulimit -l 0; build/release/tools/launch/osoe-launch -n 2 -m 0,1 -t 30 -- build/release/shmem/tests/shmem_test_internode'" \
  > "$OUT/limit-0.txt" 2>&1
grep -E "locked-memory|needs io_uring|osoe-launch" "$OUT/limit-0.txt" | head -6 >> "$LOG"
rm -rf "${OUT:?}/tree"
echo "end $(date +%H:%M:%S)" >> "$LOG"
