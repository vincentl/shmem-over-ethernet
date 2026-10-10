#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# A spot check of building.sh's container emulation: three of the tests that
# failed there, run again the same way with their output, to see that each
# failed for a container's defaults - /dev/shm too small or io_uring refused.
# usage: container-spot.sh TAR OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 3600 9 || exit 1
TAR=$1; D=$2; mkdir -p "$D"; OUT=$D/container-spot.txt; : > "$OUT"
T=/tmp/container-spot; rm -rf "${T:?}"; mkdir -p "$T"; tar -C "$T" -xf "$TAR"; cd "$T" || exit 1
cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > /dev/null 2>&1 || { echo "build failed" >> "$OUT"; exit 1; }
DENY=$(find build/release -name deny-io-uring -type f -perm -u+x | head -1)
sudo unshare -m --propagation private bash -c "
  mount -t tmpfs -o size=64m tmpfs /dev/shm || exit 1
  exec setpriv --reuid=$(id -u) --regid=$(id -g) --init-groups env HOME=$HOME PATH=$PATH \
    $DENY setup ctest --test-dir build/release --output-on-failure \
      -R '^(shmem\.barrier\.L1|shmem\.barrier-am\.0-1-2|shmem\.job\.disagree\.team-blocks)$'
" > "$D/container-spot.full" 2>&1
grep -E "osoe: .*(dev/shm|io_uring|refus)|tests passed|Test +#" "$D/container-spot.full" | cut -c1-240 >> "$OUT"
cd /; rm -rf "${T:?}"
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
