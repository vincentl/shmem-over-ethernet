#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The second read's S1 answered by a white-box test too: shmem.progress.reset-
# while-ending five times on the fixed tree (and the progress tests whose host
# gained the aborting callback), then on each library mutant, which must fail it
# every run. usage: checks-6.sh DIR (fixed.tar, unfixed-flush.tar, no-excuse.tar).
# Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 21600 9 || exit 1
D=$1; OUT=$D/checks.txt; : > "$OUT"
for name in fixed unfixed-flush no-excuse; do
  T=/tmp/helpers-checks6-$name; rm -rf "${T:?}"; mkdir -p "$T"; tar -C "$T" -xf "$D/$name.tar"
  echo "== build $name $(date +%H:%M:%S)" >> "$OUT"
  if ! (cd "$T" && cmake --preset debug > "$D/$name.configure" 2>&1 &&
        cmake --build build/debug -j4 > "$D/$name.build" 2>&1); then
    echo "BUILD FAILED $name" >> "$OUT"; grep -E "error" "$D/$name.build" | head -5 >> "$OUT"; continue
  fi
  grep -E "warning:" "$D/$name.build" | sort -u | head -3 >> "$OUT"
  if [ $name = fixed ]; then
    ctest --test-dir "$T/build/debug" -j4 --timeout 120 --output-on-failure -R '^shmem\.progress' \
      > "$D/$name-progress.full" 2>&1
    grep -E "tests passed|\*\*\*Failed|\*\*\*Timeout" "$D/$name-progress.full" >> "$OUT"
  fi
  failed=0
  for i in 1 2 3 4 5; do
    ctest --test-dir "$T/build/debug" --timeout 120 --output-on-failure \
      -R '^shmem\.progress\.reset-while-ending$' > "$D/$name-reset-$i.full" 2>&1 || failed=$((failed + 1))
  done
  echo "== $name: reset-while-ending failed $failed of 5" >> "$OUT"
  grep -h -m1 -E "osoe_fatal|RESET WHILE ENDING OK" "$D/$name"-reset-*.full | cut -c1-150 | sort | uniq -c >> "$OUT"
  rm -rf "${T:?}"
done
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
