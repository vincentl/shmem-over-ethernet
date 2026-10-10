#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# v0.2 step 4's merge point: the SOS test suite at 4 x 2 - every C test of
# unit/, spec-example/ and apps/ and the C++ ones, built with the tree's oshcc -
# at OSOE_PROGRESS_HELPERS=H (sos-sweep.sh of step D's merge point, as the
# release point ran it). usage: sos.sh TAR OUTDIR H. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 21600 9 || exit 1
TAR=$1; D=$2; H=$3; mkdir -p "$D"
S=<repo>/build/scratch/merge-d/sos-sweep.sh
rm -rf /tmp/sw2-src && mkdir -p /tmp/sw2-src && tar -C /tmp/sw2-src -xf "${TAR:?archive}"
export SWEEP_TREE=/tmp/sw2-src OSOE_PROGRESS_HELPERS=$H
OUT=$D/sos.txt
echo "== SOS whole, 4 x 2, OSOE_PROGRESS_HELPERS=$H $(date +%H:%M:%S)" >> "$OUT"
SWEEP_OUT=$D/sos-4x2-h$H.txt SWEEP_MAPS=0,0,1,1,2,2,3,3 bash $S
tail -4 "$D/sos-4x2-h$H.txt" >> "$OUT"
rm -rf /tmp/sw2 /tmp/sw2work /tmp/sw2-src
echo "DONE h$H $(date +%H:%M:%S)" >> "$OUT"
