#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The fast-link kit's dry run in the VM: the tree unpacked, built with the kit's
# build.sh under osoe-launch, then docs/fast-link/dry-run.sh. Takes the VM lock.
# usage: dry-run-vm.sh TAR OUTDIR
exec 9>/tmp/osoe-vm.lock; flock -w 43200 9 || exit 1
TAR=$1; D=$2; mkdir -p "$D"
T=/tmp/fast-link-dry-run; rm -rf "${T:?}"; mkdir -p "$T"; tar -C "$T" -xf "$TAR"; cd "$T" || exit 1
printf 'one\n' > "$D/one-node"
KIT_LAUNCHER=osoe-launch OUT="$D/build" bash docs/fast-link/build.sh "$D/one-node" > "$D/build.txt" 2>&1
echo "build: $?" >> "$D/build.txt"
bash docs/fast-link/dry-run.sh "$D/dry-run" > "$D/dry-run.txt" 2>&1
cd /; rm -rf "${T:?}"
echo "DONE $(date +%H:%M:%S)" >> "$D/dry-run.txt"
