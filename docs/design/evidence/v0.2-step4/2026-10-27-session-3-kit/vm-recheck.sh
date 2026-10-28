#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# After the chain's dry run, under the VM's lock: the tree (TAR, a git archive
# of COMMIT) unpacked into ~/kit-s3/src2 - a directory new to this run, which it
# stops at if it is there already - and built by the kit's build.sh; H12's wake
# point alone at 64 PEs a node, its profile by sudo -n perf as on the cluster;
# the kit's own dry run, every section at small sizes - the paths the chain does
# not take, floor.sh's three parts and window.sh's own cases among them; and the
# static checks. What to bring back goes to COPY/recheck.
# usage: vm-recheck.sh TAR COMMIT COPY
set -u
TAR=$1; COMMIT=$2; COPY=$3/recheck
T=$HOME/kit-s3/src2
R=$HOME/kit-s3/recheck
exec 9>/tmp/osoe-vm.lock
echo "# waiting for the lock $(date -u +%FT%TZ)"
flock -w 43200 9 || { echo "no lock"; exit 1; }
echo "# lock taken $(date -u +%FT%TZ); load $(cut -d' ' -f1-3 /proc/loadavg)"
[ -e "$T" ] || [ -e "$R" ] && { echo "$T or $R is there already: not overwritten"; exit 1; }
mkdir -p "$T" "$R" "$COPY"
tar -C "$T" -xf "$TAR" || exit 1
cd "$T" || exit 1
printf 'one\n' > "$R/one-node"
KIT_COMMIT=$COMMIT KIT_LAUNCHER=osoe-launch OUT="$R/build" \
    bash docs/fast-link/build.sh "$R/one-node" > "$R/build.txt" 2>&1
echo "build.sh: exit $?"
printf 'one\ntwo\n' > "$R/pair"
KIT_COMMIT=$COMMIT KIT_LAUNCHER=osoe-launch PERF="sudo -n perf" PES_PER_NODE=" " WAKE_PES=64 \
    bash docs/fast-link/many-pes.sh "$R/pair" "$R/wake" > "$R/wake.log" 2>&1
echo "many-pes.sh, the wake point alone: exit $?"
python3 docs/fast-link/summarize.py "$R/wake" > "$R/wake-summary.txt" 2>&1
cat "$R/wake/profile-wake-pes64-h0-r1.txt"
began=$(date +%s)
KIT_COMMIT=$COMMIT bash docs/fast-link/dry-run.sh "$R/std" > "$R/std.txt" 2>&1
echo "dry-run.sh: exit $?, $(( ($(date +%s) - began) / 60 )) min"
cat "$R/std.txt"
ctest --test-dir build/release -j4 --timeout 600 -R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|settings-known|pass-expressions|whole-lines|license-lines|single-node\.configure|version\.)' \
    > "$R/static.txt" 2>&1
echo "the static checks: $(grep -E 'tests passed' "$R/static.txt")"
cp "$R"/*.txt "$R"/*.log "$COPY/" 2>/dev/null
cp "$R/wake/run.txt" "$COPY/wake-run.txt"
cp "$R"/wake/profile-*.txt "$COPY/" 2>/dev/null
cp "$R"/std/*-summary.txt "$COPY/" 2>/dev/null
echo "# done $(date -u +%FT%TZ)"
