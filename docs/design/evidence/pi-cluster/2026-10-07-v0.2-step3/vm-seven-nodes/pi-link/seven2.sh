#!/bin/bash
# v0.2 step 3 Part 3, the VM's half again: seven logical nodes of one PE, the
# shapes fixed by setting the Pi's link - `link` takes Träff's (a window of 12)
# from 8 KiB, `tree` the trees (a fan-out of 2 at 1 MiB) - measured on loopback,
# alternating three times; and three starts with the link unset, for the
# loopback's own numbers. Usage: seven2.sh TAR OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
TAR=$1; D=$2; mkdir -p $D; OUT=$D/seven.txt; : > $OUT
T=/tmp/v02-circ-seven2-$$; rm -rf $T; mkdir -p $T; tar -C $T -xf $TAR; cd $T
cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > $D/build.txt 2>&1 ||
  { echo "build failed" >> $OUT; exit 1; }
O=$T/build/release/tools/wrapper/oshcc
L=$T/build/release/tools/launch/osoe-launch
$O -O2 -o $T/e-band /Users/vincent/shmem-over-ethernet/docs/design/evidence/pi-cluster/2026-10-04-step-e/e-band.c || { echo "e-band build failed" >> $OUT; exit 1; }
PI="OSOE_LINK_HOP_NS=86000 OSOE_LINK_MESSAGE_NS=7000 OSOE_LINK_BYTES_PER_SECOND=116000000"
for i in 1 2 3; do
  for side in link tree; do
    echo "== $side $i $(date +%H:%M:%S)" >> $OUT
    env $PI OSOE_COLLECTIVE_SHAPE=$side $L -n 7 -m 0,1,2,3,4,5,6 -t 600 -- $T/e-band > $D/band-7-$side-$i.txt 2>&1
    echo "exit $?" >> $OUT
  done
done
for i in 1 2 3; do
  SHMEM_DEBUG=1 $L -n 7 -m 0,1,2,3,4,5,6 -t 600 -- $T/e-band > $D/measured-$i.txt 2>&1
  grep -h 'PE 0 of 7' $D/measured-$i.txt | sed 's/.*link measured: //' >> $OUT
done
cd /; rm -rf $T
echo "DONE $(date +%H:%M:%S)" >> $OUT
