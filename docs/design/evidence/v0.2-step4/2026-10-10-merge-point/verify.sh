#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# v0.2 step 4's merge point, the fixes' tree: the static checks first, then
# Part 3's verification - the four presets at no helpers with the
# floor pins on (release without the models); debug, release, asan and tsan at 2 and 3
# helpers (tsan's failures gated test by test by tsan-gate.py: each by TSan's
# reports alone, every one a landing in a heap through another process, and
# stopped-bulk's timeout explained - TSan's slow path over 64 MiB of racy reads,
# checks-5); tier 3; the models one at a time; osoe-pacer's mix and mailbox a
# minute each at 0 and 2 helpers, its collectives with the Pi's link at 0 and 2,
# and ten minutes of mix at each.
# Usage: verify.sh TAR OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
TAR=$1; D=$2; S=$(dirname "$0")
mkdir -p $D; OUT=$D/verify.txt; : > $OUT
T=/tmp/v02-step4-merge-verify; rm -rf "${T:?}"; mkdir -p $T; tar -C $T -xf $TAR; cd $T || exit 1
suite() {   # preset, environment, extra ctest arguments
  p=$1; e=$2; shift 2
  tag=$p-$(echo "$e" | tr ',=' '__')
  echo "== $p $e $(date +%H:%M:%S)" >> $OUT
  env $(echo "$e" | tr ',' ' ') ctest --test-dir build/$p -j4 --timeout 600 --output-on-failure "$@" > $D/$tag.full 2>&1
  grep -E "tests passed|\*\*\*Failed|\*\*\*Timeout|Not Run|\(Failed\)|\(Timeout\)" $D/$tag.full >> $OUT
}
echo "== static checks $(date +%H:%M:%S)" >> $OUT
cmake --preset release -DOSOE_TEST_PINS=ON > /dev/null 2>&1 || { echo "configure failed" >> $OUT; exit 1; }
cmake --build build/release -j4 > $D/build-static.txt 2>&1 || { echo "BUILD FAILED release" >> $OUT; exit 1; }
ctest --test-dir build/release -j4 --output-on-failure \
  -R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|license-lines|version\.)' \
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
  case $p in
    debug|asan) suite $p OSOE_PROGRESS_HELPERS=0; suite $p OSOE_PROGRESS_HELPERS=2
                suite $p OSOE_PROGRESS_HELPERS=3 ;;
    release)    suite $p OSOE_PROGRESS_HELPERS=0 -LE '^model$'
                suite $p OSOE_PROGRESS_HELPERS=2 -LE '^model$'
                suite $p OSOE_PROGRESS_HELPERS=3 -LE '^model$' ;;
    tsan)       suite $p OSOE_PROGRESS_HELPERS=0; suite $p OSOE_PROGRESS_HELPERS=2
                suite $p OSOE_PROGRESS_HELPERS=3
                for h in 2 3; do
                  python3 $S/tsan-gate.py --timeout-explained shmem.stopped-bulk.0-0-1 \
                    $D/tsan-OSOE_PROGRESS_HELPERS_$h.full >> $OUT 2>&1
                done ;;
  esac
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
cd /; rm -rf "${T:?}"
echo "DONE $(date +%H:%M:%S)" >> $OUT
