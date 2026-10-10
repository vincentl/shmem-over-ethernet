#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# v0.2 step 4 part 2: each mutant tree - one change to the branch - built in
# debug and run against the tests of what it breaks (REGEX), which should fail
# on it; the first failing tests and the messages they ended with.
# usage: mutants.sh DIR REGEX (DIR holds NAME.tar for each mutant). Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
D=$1; R=$2; OUT=$D/mutants.txt; : > "$OUT"
for tar in "$D"/*.tar; do
  name=$(basename "$tar" .tar)
  T=/tmp/control-wire-mutant-$name; rm -rf "${T:?}"; mkdir -p "$T"; tar -C "$T" -xf "$tar"; cd "$T" || exit 1
  echo "== $name $(date +%H:%M:%S)" >> "$OUT"
  cmake --preset debug > "$D/$name.configure" 2>&1
  if ! cmake --build build/debug -j4 > "$D/$name.build" 2>&1; then
    echo "BUILD FAILED" >> "$OUT"; grep -E "error" "$D/$name.build" | head -5 >> "$OUT"
  else
    ctest --test-dir build/debug -j4 --timeout 120 --output-on-failure -R "$R" > "$D/$name.full" 2>&1
    grep -E "tests passed" "$D/$name.full" >> "$OUT"
    grep -E "\*\*\*Failed|\*\*\*Timeout|Not Run" "$D/$name.full" | sed -E 's/^ *[0-9]+\/[0-9]+ //' | head -12 >> "$OUT"
    grep -m4 -E "osoe_fatal|osoe: .*node [0-9]+:|CHECK\(|buffers were held|in flight|stopped answering|no such job" "$D/$name.full" >> "$OUT"
  fi
  cd / || exit 1; rm -rf "${T:?}"
done
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
