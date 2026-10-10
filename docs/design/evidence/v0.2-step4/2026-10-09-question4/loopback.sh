#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Question 4 (v0.2, 2026-10-09): get-pieces.c over the VM's loopback - two PEs on
# two logical nodes, a release build of main - three runs of 21 a size, then the
# same at two helpers once. usage: loopback.sh TAR OUT. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 21600 9 || exit 1
TAR=$1; OUT=$2; Q=$(dirname "$0"); : > "$OUT"
T=/tmp/question4-loopback; rm -rf "${T:?}"; mkdir -p "$T"; tar -C "$T" -xf "$TAR"; cd "$T" || exit 1
cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > /dev/null 2>&1 || { echo "build failed" >> "$OUT"; exit 1; }
build/release/tools/wrapper/oshcc -O2 -o get-pieces "$Q/get-pieces.c" || { echo "oshcc failed" >> "$OUT"; exit 1; }
for i in 1 2 3; do
  echo "run $i at no helpers $(date +%H:%M:%S)" >> "$OUT"
  build/release/tools/launch/osoe-launch -n 2 -m 0,1 -t 120 -- ./get-pieces 21 | grep "^bytes=" >> "$OUT"
done
echo "run at two helpers $(date +%H:%M:%S)" >> "$OUT"
OSOE_PROGRESS_HELPERS=2 build/release/tools/launch/osoe-launch -n 2 -m 0,1 -t 120 -- ./get-pieces 21 | grep "^bytes=" >> "$OUT"
cd /; rm -rf "${T:?}"
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
