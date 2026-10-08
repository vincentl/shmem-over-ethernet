#!/bin/bash
# v0.2 step 4 part 0, the zero-copy send's verification on osoe: the four presets with the
# floor pins on (release without the models), tier 3, and osoe-pacer's mix (ten
# minutes), mailbox and collectives (a minute each). usage: verify.sh TAR OUTDIR.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
TAR=$1; D=$2; mkdir -p $D; OUT=$D/verify.txt; : > $OUT
T=/tmp/ladder-verify; rm -rf $T; mkdir -p $T; tar -C $T -xf $TAR; cd $T
for p in debug release asan tsan; do
  echo "== $p $(date +%H:%M:%S)" >> $OUT
  cmake --preset $p -DOSOE_TEST_PINS=ON > /dev/null 2>&1 || { echo "configure failed" >> $OUT; continue; }
  if ! cmake --build build/$p -j4 > $D/$p.build 2>&1; then echo "BUILD FAILED" >> $OUT; continue; fi
  grep -E "warning:|error:" $D/$p.build | sort -u | head >> $OUT
  sel=""; [ $p = release ] && sel="-LE ^model$"
  ctest --test-dir build/$p -j4 --timeout 300 --output-on-failure $sel > $D/$p.full 2>&1
  grep -E "tests passed|\*\*\*Failed|\*\*\*Timeout|Not Run" $D/$p.full >> $OUT
  [ $p != release ] && rm -rf build/$p
done
echo "== tier3 $(date +%H:%M:%S)" >> $OUT
cmake --preset tier3 > /dev/null 2>&1 && cmake --build build/tier3 -j4 > /dev/null 2>&1
ctest --preset tier3 --test-dir build/tier3 --timeout 1500 --output-on-failure > $D/tier3.full 2>&1
grep -E "tests passed|\*\*\*Failed|\*\*\*Timeout" $D/tier3.full >> $OUT
rm -rf build/tier3
L=$T/build/release/tools/launch/osoe-launch
P=$T/build/release/tools/pacer/osoe-pacer
echo "== pacer mailbox $(date +%H:%M:%S)" >> $OUT
$L -n 8 -m 0,0,1,1,2,2,3,3 -t 300 -- $P --seconds 60 mailbox > $D/pacer-mailbox.txt 2>&1
echo "exit $? $(tail -1 $D/pacer-mailbox.txt)" >> $OUT
echo "== pacer collectives $(date +%H:%M:%S)" >> $OUT
$L -n 6 -m 0,0,1,1,2,2 -t 300 -- $P --seconds 60 collectives > $D/pacer-collectives.txt 2>&1
echo "exit $? $(tail -1 $D/pacer-collectives.txt)" >> $OUT
echo "== pacer mix, ten minutes $(date +%H:%M:%S)" >> $OUT
$L -n 8 -m 0,0,1,1,2,2,3,3 -t 900 -- $P --seconds 600 mix > $D/pacer-mix.txt 2>&1
echo "exit $? $(tail -1 $D/pacer-mix.txt)" >> $OUT
echo "== pacer mix with the zero-copy send on, ten minutes $(date +%H:%M:%S)" >> $OUT
OSOE_WIRE_ZERO_COPY=on OSOE_WIRE_ZERO_COPY_BYTES=1024 $L -n 8 -m 0,0,1,1,2,2,3,3 -t 900 -- $P --seconds 600 mix > $D/pacer-mix-zero-copy.txt 2>&1
echo "exit $? $(tail -1 $D/pacer-mix-zero-copy.txt)" >> $OUT
echo "== pacer mailbox with the zero-copy send on $(date +%H:%M:%S)" >> $OUT
OSOE_WIRE_ZERO_COPY=on OSOE_WIRE_ZERO_COPY_BYTES=1024 $L -n 8 -m 0,0,1,1,2,2,3,3 -t 300 -- $P --seconds 60 mailbox > $D/pacer-mailbox-zero-copy.txt 2>&1
echo "exit $? $(tail -1 $D/pacer-mailbox-zero-copy.txt)" >> $OUT
cd /; rm -rf $T
echo "DONE $(date +%H:%M:%S)" >> $OUT
