#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The third session's kit in the osoe VM, under the VM's lock: the tree (TAR, a
# git archive of COMMIT) unpacked into ~/kit-s3/src, the kit's scripts' syntax,
# then dry-run.sh's session-3 - the chain whole, at its own settings on five
# logical nodes, H12's 256 PEs a node at 64, the four trees of §16 from
# TREE_TARS (git archive's tars, the VM having no git), SOS's sources from
# SOS_SOURCE - with /dev/shm's use and the memory sampled every 10 s beside it;
# then the static checks on the build the chain made. What to bring back is
# copied to COPY (the chain's lines and summaries, every section's log and
# run.txt, the samples).
# usage: vm-dry-run.sh TAR TREE_TARS SOS_SOURCE COMMIT COPY
set -u
TAR=$1; TREE_TARS=$2; SOS_SOURCE=$3; COMMIT=$4; COPY=$5
T=$HOME/kit-s3
exec 9>/tmp/osoe-vm.lock
echo "# waiting for the lock $(date -u +%FT%TZ)"
flock -w 43200 9 || { echo "no lock"; exit 1; }
echo "# lock taken $(date -u +%FT%TZ); load $(cut -d' ' -f1-3 /proc/loadavg)"
vmstat 5 2 | tail -1
free -m | sed -n 2p
df -h /tmp /dev/shm | tail -2
rm -rf "$T"
mkdir -p "$T/src" "$COPY"
tar -C "$T/src" -xf "$TAR" || exit 1
cd "$T/src" || exit 1
for s in docs/fast-link/*.sh; do bash -n "$s" || echo "$s: syntax"; done
python3 -m py_compile docs/fast-link/summarize.py && echo "the scripts parse"
( while sleep 10; do
      echo "$(date -u +%H:%M:%S) shm_used=$(df -B1 /dev/shm | awk 'NR == 2 { print $3 }')" \
           "mem_available=$(awk '/^MemAvailable/ { print $2 * 1024 }' /proc/meminfo)" \
           "load=$(cut -d' ' -f1 /proc/loadavg)"
  done ) > "$T/samples.txt" 2>&1 &
sampler=$!
began=$(date +%s)
KIT_COMMIT=$COMMIT SECTIONS=session-3 TREE_TARS=$TREE_TARS SOS_SOURCE=$SOS_SOURCE \
    bash docs/fast-link/dry-run.sh "$T/dry" > "$T/dry-run.txt" 2>&1
echo "dry-run.sh: exit $?, $(( ($(date +%s) - began) / 60 )) min"
kill "$sampler"
cat "$T/dry-run.txt"
echo "the most of /dev/shm used: $(awk '{ sub("shm_used=", "", $2); if ($2 > m) m = $2 } END { print m }' "$T/samples.txt") bytes;" \
     "the least memory available: $(awk '{ sub("mem_available=", "", $3); if (m == "" || $3 < m) m = $3 } END { print m }' "$T/samples.txt") bytes"
ctest --test-dir build/release -j4 --timeout 600 -R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|settings-known|pass-expressions|whole-lines|license-lines|single-node\.configure|version\.)' \
    > "$T/static.txt" 2>&1
echo "the static checks: $(grep -E 'tests passed' "$T/static.txt")"
grep -E '\(Failed\)' "$T/static.txt"
S=$T/dry/session-3
cp "$T/dry-run.txt" "$T/samples.txt" "$T/static.txt" "$COPY/"
cp "$S"/sections.txt "$S"/summary.txt "$S"/*.log "$S"/*-summary.txt "$COPY/" 2>/dev/null
for d in "$S"/*/; do
    n=$(basename "$d")
    [ -e "$d/run.txt" ] && cp "$d/run.txt" "$COPY/$n-run.txt"
done
cp "$S"/setup/verdicts.txt "$COPY/setup-verdicts.txt" 2>/dev/null
cp "$S"/scale/sos-build.txt "$COPY/sos-build.txt" 2>/dev/null
echo "# done $(date -u +%FT%TZ)"
