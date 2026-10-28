#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# T38's calibration (burst.c): a fixed burst's CPU a gap apart and back to back,
# alone and beside 1, 4 and 8 busy loops. Takes the VM's lock.
#   calibrate.sh OUT
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$1
exec 9>/tmp/osoe-vm.lock
flock -w 3600 9 || { echo "the VM's lock was not had in an hour"; exit 1; }
mkdir -p /tmp/t38 && gcc -O2 -o /tmp/t38/burst "$HERE/burst.c" || exit 2
SPINNERS=""
spin() { for i in $(seq 1 "$1"); do ( exec 9>&-; while :; do :; done ) & SPINNERS="$SPINNERS $!"; done; }
unspin() { for q in $SPINNERS; do kill "$q" 2>/dev/null; done; wait 2>/dev/null; SPINNERS=""; }
trap unspin EXIT
: > "$OUT"
for loads in 0 1 4 8; do
    spin "$loads"
    sleep 1
    for run in 1 2 3; do
        echo "loads $loads run $run: $(/tmp/t38/burst 20000 300 2000)" | tee -a "$OUT"
    done
    unspin
done
