#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The kit on P9's branch, in the osoe VM, under the VM's lock: the tree (TAR,
# a git archive of COMMIT) built by the kit's own build.sh under osoe-launch -
# the release preset and the probes, claims among them - the static checks on
# that build, the scripts' syntax, then the kit's dry run (dry-run.sh); then
# the tree before P9 (BEFORE_TAR, 821a6dfb) built with claims.c compiled by
# its own oshcc, and claims.sh run with BUILD=0 over both builds.
# usage: p9-kit-dry.sh TAR BEFORE_TAR COMMIT
set -u
TAR=$1; BEFORE_TAR=$2; COMMIT=$3
T=$HOME/kit-p9
exec 9>/tmp/osoe-vm.lock
flock -w 43200 9 || { echo "no lock"; exit 1; }
echo "# lock taken $(date -u +%FT%TZ); $(cut -d' ' -f1-3 /proc/loadavg)"
rm -rf "$T"; mkdir -p "$T/src" "$T/trees/821a6dfb"
tar -C "$T/src" -xf "$TAR" || exit 1
tar -C "$T/trees/821a6dfb" -xf "$BEFORE_TAR" || exit 1
cd "$T/src" || exit 1
printf 'one\n' > "$T/one-node"
KIT_COMMIT=$COMMIT KIT_LAUNCHER=osoe-launch OUT="$T/build-log" \
    bash docs/fast-link/build.sh "$T/one-node" > "$T/build.txt" 2>&1
status=$?
echo "build.sh: $status; warnings in the release build: $(grep -c 'warning:' build-release.txt)"
[ "$status" -eq 0 ] || exit "$status"
ls build/release/kit
ctest --test-dir build/release -j4 --timeout 600 -R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|settings-known|pass-expressions|whole-lines|license-lines|single-node\.configure|version\.)' > "$T/static.txt" 2>&1
echo "static: $(grep -E 'tests passed' "$T/static.txt")"
for s in docs/fast-link/*.sh; do bash -n "$s" || echo "$s: syntax"; done
python3 -m py_compile docs/fast-link/summarize.py && echo "the scripts parse"
export KIT_COMMIT=$COMMIT
bash docs/fast-link/dry-run.sh "$T/dry" > "$T/dry-run.txt" 2>&1
echo "dry-run.sh: $?"; cat "$T/dry-run.txt"
# The before path: the tree before P9 built, claims compiled by its oshcc.
( cd "$T/trees/821a6dfb" && { cmake --preset release && cmake --build build/release -j4; } > build-release.txt 2>&1 &&
  mkdir -p build/release/kit &&
  build/release/tools/wrapper/oshcc -O2 -o build/release/kit/claims "$T/src/docs/fast-link/claims.c" >> build-release.txt 2>&1 )
echo "before tree: $? warnings $(grep -c 'warning:' "$T/trees/821a6dfb/build-release.txt")"
printf 'one\ntwo\n' > "$T/nodes"
KIT_LAUNCHER=osoe-launch KIT_TIMEOUT=300 SHMEM_SYMMETRIC_SIZE=16m BUILD=0 TREES="$T/trees" \
    ROUNDS=2 PES_PER_NODE="2 4" KEEPS="1 2" ITERATIONS=100 \
    bash docs/fast-link/claims.sh "$T/nodes" "$T/claims" > "$T/claims.log" 2>&1
echo "claims.sh: $?; jobs that failed: $(grep -c 'exit [1-9]' "$T/claims/run.txt")"
python3 docs/fast-link/summarize.py "$T/claims"
echo "# done $(date -u +%FT%TZ)"
