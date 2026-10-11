#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The full verification of round nine's changes before they merge, as the
# integration's of 2026-10-10 (verify-integration.sh), with ThreadSanitizer's
# verdict now inside ctest (cmake/OsoeThreadSanitizer.cmake): the static checks
# first; the four presets at no helpers, two and three with the floor pins on
# (release without the models), ThreadSanitizer's reports judged by its own
# test, tsan.reports, and the gate run again on the files for its breakdown and
# the files kept; tier 3; the models one at a time; osoe-pacer's mix and
# mailbox a minute each at 0 and 2 helpers, its collectives with the Pi's link
# at 0 and 2, and ten minutes of mix at each; the single-node build.
# Skipped tests are listed too: a test that skips reads as green otherwise.
# Usage: verify-fixes.sh TAR OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 43200 9 || exit 1
TAR=$1; D=$2
mkdir -p $D; OUT=$D/verify.txt; : > $OUT
T=/tmp/r9f-verify; rm -rf "${T:?}"; mkdir -p $T; tar -C $T -xf $TAR 2>/dev/null; cd $T || exit 1
suite() {   # preset, environment, extra ctest arguments
  p=$1; e=$2; shift 2
  tag=$p-$(echo "$e" | tr ',=' '__')
  echo "== $p $e $(date +%H:%M:%S)" >> $OUT
  env $(echo "$e" | tr ',' ' ') ctest --test-dir build/$p -j4 --timeout 600 --output-on-failure "$@" > $D/$tag.full 2>&1
  grep -E "tests passed|\*\*\*Failed|\*\*\*Timeout|\*\*\*Skipped|Not Run|\(Failed\)|\(Timeout\)|\(Skipped\)" $D/$tag.full >> $OUT
}
echo "== static checks $(date +%H:%M:%S)" >> $OUT
cmake --preset release -DOSOE_TEST_PINS=ON > /dev/null 2>&1 || { echo "configure failed" >> $OUT; exit 1; }
cmake --build build/release -j4 > $D/build-static.txt 2>&1 || { echo "BUILD FAILED release" >> $OUT; exit 1; }
ctest --test-dir build/release -j4 --output-on-failure \
  -R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|license-lines|single-node\.configure|version\.)' \
  > $D/static.full 2>&1
grep -E "tests passed|\(Failed\)" $D/static.full >> $OUT
grep -q "100% tests passed" $D/static.full || { echo "STATIC CHECKS FAILED" >> $OUT; exit 1; }
for p in debug release asan tsan; do
  echo "== build $p $(date +%H:%M:%S)" >> $OUT
  cmake --preset $p -DOSOE_TEST_PINS=ON > /dev/null 2>&1 || { echo "configure failed" >> $OUT; continue; }
  if ! cmake --build build/$p -j4 > $D/build-$p.txt 2>&1; then
    echo "BUILD FAILED $p" >> $OUT; grep -E "error" $D/build-$p.txt | head -5 >> $OUT; continue
  fi
  grep -E "warning:" $D/build-$p.txt | sort -u | head >> $OUT
  for h in 0 2 3; do
    case $p in
      release) suite $p OSOE_PROGRESS_HELPERS=$h -LE '^model$' ;;
      *)       suite $p OSOE_PROGRESS_HELPERS=$h ;;
    esac
    if [ $p = tsan ]; then
      python3 -I cmake/thread-sanitizer-gate.py judge build/tsan/thread-sanitizer-reports --ctest ctest \
        --build build/tsan --place build/tsan/osoe_thread_sanitizer_place > $D/tsan-gate-$h.txt 2>&1
      grep -E '^  passed [0-9]|report files|GATE' $D/tsan-gate-$h.txt >> $OUT
      rm -rf $D/tsan-reports-$h; cp -r build/tsan/thread-sanitizer-reports $D/tsan-reports-$h
    fi
  done
  [ $p != release ] && rm -rf build/$p
done
echo "== tier3 $(date +%H:%M:%S)" >> $OUT
cmake --preset tier3 > /dev/null 2>&1 && cmake --build build/tier3 -j4 > /dev/null 2>&1
ctest --preset tier3 --test-dir build/tier3 --timeout 1500 --output-on-failure > $D/tier3.full 2>&1
grep -E "tests passed|\*\*\*Failed|\*\*\*Timeout" $D/tier3.full >> $OUT
rm -rf build/tier3
echo "== models $(date +%H:%M:%S)" >> $OUT
ctest --test-dir build/release -j1 -L '^model$' --timeout 3600 --output-on-failure > $D/models.full 2>&1
grep -E "tests passed|\*\*\*Failed|\*\*\*Timeout|Not Run|\(Failed\)" $D/models.full >> $OUT
L=$T/build/release/tools/launch/osoe-launch
P=$T/build/release/tools/pacer/osoe-pacer
PI="OSOE_LINK_HOP_NS=86000 OSOE_LINK_MESSAGE_NS=7000 OSOE_LINK_BYTES_PER_SECOND=116000000"
pacer() {   # label, helpers, seconds, launcher arguments..., then the pacer's
  label=$1; h=$2; shift 2
  echo "== pacer $label at $h helpers $(date +%H:%M:%S)" >> $OUT
  env OSOE_PROGRESS_HELPERS=$h "$@" > $D/pacer-$label-$h.txt 2>&1
  echo "exit $? $(grep -o 'osoe-pacer failures=[0-9]*' $D/pacer-$label-$h.txt | tail -1) $(grep -c 'osoe: deadlock' $D/pacer-$label-$h.txt) deadlock lines" >> $OUT
}
for h in 0 2; do
  pacer mix $h $L -n 8 -m 0,0,1,1,2,2,3,3 -t 300 -- $P --seconds 60 mix
  pacer mailbox $h $L -n 8 -m 0,0,1,1,2,2,3,3 -t 300 -- $P --seconds 60 mailbox
  pacer collectives $h env $PI $L -n 6 -m 0,0,1,1,2,2 -t 600 -- $P --seconds 60 collectives
  pacer mix-ten-minutes $h env $PI $L -n 8 -m 0,0,1,1,2,2,3,3 -t 900 -- $P --seconds 600 mix
done
echo "== the single-node build $(date +%H:%M:%S)" >> $OUT
cd $T && cmake -S . -B build/single-node -DCMAKE_BUILD_TYPE=Debug -DOSOE_SINGLE_NODE=ON > $D/configure-single-node.txt 2>&1 \
  && cmake --build build/single-node -j4 > $D/build-single-node.txt 2>&1 \
  || echo "BUILD FAILED single-node" >> $OUT
echo "warnings: $(grep -c 'warning:' $D/build-single-node.txt)" >> $OUT
ctest --test-dir build/single-node -j4 -LE '^model$' --timeout 600 --output-on-failure > $D/single-node.full 2>&1
grep -E "tests passed|\(Failed\)|\(Timeout\)|\(Skipped\)" $D/single-node.full >> $OUT
cd /; rm -rf "${T:?}"
echo "DONE $(date +%H:%M:%S)" >> $OUT
