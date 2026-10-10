#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The full build's debug suite where io_uring is refused, as the README's
# requirements section describes it: ctest under `deny-io-uring setup`, whose
# seccomp filter every test inherits and which refuses io_uring_setup with
# EPERM as kernel.io_uring_disabled=2 does - without touching the VM's sysctl.
# Lists the failures, the internode-labelled tests, and the two differences.
# Usage: refused-count.sh TAR OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 21600 9 || exit 1
TAR=$1; D=$2
mkdir -p "$D"; OUT=$D/refused.txt; : > "$OUT"
T=/tmp/v02-helpers-refused; rm -rf "${T:?}"; mkdir -p "$T"; tar -C "$T" -xf "$TAR"; cd "$T" || exit 1
echo "== build $(date +%H:%M:%S)" >> "$OUT"
cmake --preset debug > /dev/null 2>&1 || { echo "configure failed" >> "$OUT"; exit 1; }
cmake --build build/debug -j4 > "$D/build.txt" 2>&1 || { echo "BUILD FAILED" >> "$OUT"; exit 1; }
DENY=$(find build/debug -name deny-io-uring -type f -perm -u+x | head -1)
[ -n "$DENY" ] || { echo "no deny-io-uring" >> "$OUT"; exit 1; }
ctest --test-dir build/debug -N -L internode | sed -n 's/^ *Test *#[0-9]*: //p' | sort > "$D/internode.txt"
ctest --test-dir build/debug -N | sed -n 's/^ *Test *#[0-9]*: //p' | sort > "$D/all.txt"
echo "== suite under deny-io-uring setup $(date +%H:%M:%S)" >> "$OUT"
"$DENY" setup ctest --test-dir build/debug -j4 --timeout 600 > "$D/suite.full" 2>&1
grep -E "tests passed" "$D/suite.full" >> "$OUT"
sed -n 's/^[[:space:]]*[0-9][0-9]* - \(.*\) ([^)]*)$/\1/p' "$D/suite.full" | sort > "$D/failed.txt"
echo "failed $(wc -l < "$D/failed.txt"); internode $(wc -l < "$D/internode.txt"); all $(wc -l < "$D/all.txt")" >> "$OUT"
echo "failed, not internode:" >> "$OUT"; comm -23 "$D/failed.txt" "$D/internode.txt" >> "$OUT"
comm -13 "$D/failed.txt" "$D/internode.txt" > "$D/internode-passed.txt"
echo "internode, passed: $(wc -l < "$D/internode-passed.txt")" >> "$OUT"
grep -E "\(Timeout\)" "$D/suite.full" | head >> "$OUT"
cd /; rm -rf "${T:?}"
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
