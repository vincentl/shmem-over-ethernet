#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §18: P9's timing (docs/prompts/v0.2-open-items.md) - leads
# claiming team blocks of one node's pool at once, before P9's change and after
# it. Every PE of one node splits SHMEM_TEAM_SELF KEEP times at a start PE 0
# sets, and so claims KEEP blocks (claims.c), ITERATIONS times. BEFORE is the
# commit the change was built on: its tree is written into TREES/BEFORE by git
# archive from this checkout, which needs its .git, copied to the list's first
# node unless this is it, and built there alone, as trees.sh builds Part 0's,
# with claims.c compiled by that tree's own oshcc; BUILD=0 takes the tree TREES
# holds, built, and BEFORE empty runs the kit's build alone. Then on the first
# node, at each count of PES_PER_NODE and each of KEEPS, the builds in another
# order each round, ROUNDS rounds; each case's files carry "before" or "after".
#   BEFORE (821a6dfb), TREES (build/trees under the repository), BUILD (1),
#   ROUNDS (3), PES_PER_NODE ("16 32 64"), KEEPS ("1 8"), ITERATIONS (1000)
# usage: claims.sh NODES OUTDIR
: "${KIT_TIMEOUT:=300}"
source "$(dirname "$0")/common.sh"
kit_start "$1" "$2"
BEFORE=${BEFORE-821a6dfb}
TREES=${TREES:-$REPOSITORY/build/trees}
ROUNDS=${ROUNDS:-3}
PES_PER_NODE=${PES_PER_NODE:-16 32 64}
KEEPS=${KEEPS:-1 8}
ITERATIONS=${ITERATIONS:-1000}
build_before() {
    ( cd "$TREES/$BEFORE" || exit 1
      { cmake --preset release && cmake --build build/release -j"$(nproc)"; } \
          > build-release.txt 2>&1 || { echo "build failed"; exit 1; }
      mkdir -p build/release/kit
      build/release/tools/wrapper/oshcc -O2 -o build/release/kit/claims \
          "$REPOSITORY/docs/fast-link/claims.c" >> build-release.txt 2>&1 ||
          { echo "claims did not compile"; exit 1; }
      echo "built, $(grep -c 'warning:' build-release.txt) warnings" ) 2>&1 |
        sed "s/^/$(hostname): $BEFORE: /"
    [ "${PIPESTATUS[0]}" -eq 0 ] || { rm -rf "$TREES/$BEFORE/build"; return 1; }
}
if [ -n "$BEFORE" ] && [ "${BUILD:-1}" = 1 ]; then
    rm -rf "${TREES:?}/$BEFORE"
    mkdir -p "$TREES/$BEFORE"
    git -C "$REPOSITORY" archive "$BEFORE" | tar -x -C "$TREES/$BEFORE" ||
        { echo "no tree for $BEFORE: git archive needs this checkout's .git" | tee -a "$LOG"; exit 1; }
    if [ "$KIT_LAUNCHER" = osoe-launch ]; then
        build_before | tee -a "$LOG"
        [ "${PIPESTATUS[0]}" -eq 0 ] || exit 1
    else
        first=$(node 1)
        if [ "$first" != "$(hostname -s)" ]; then
            ssh "$first" mkdir -p "$TREES" &&
                rsync -a --delete "$TREES/$BEFORE"/ "$first:$TREES/$BEFORE"/ ||
                { echo "copying the tree to $first failed" | tee -a "$LOG"; exit 1; }
        fi
        export -f build_before
        export BEFORE TREES REPOSITORY
        timeout -k 30 1800 srun -J osoe-kit-build -N1 -w "$first" --ntasks-per-node=1 \
            --cpus-per-task="$(nproc)" bash -c build_before 2>&1 < /dev/null | tee -a "$LOG"
        [ "${PIPESTATUS[0]}" -eq 0 ] || { echo "the tree's build failed" | tee -a "$LOG"; exit 1; }
    fi
fi
one=
[ "$KIT_LAUNCHER" = osoe-launch ] || one="-w $(node 1)"
for ((round = 1; round <= ROUNDS; round++)); do
    for tree in $(rotate "${BEFORE:+before }after" "$round"); do
        build=$KIT_BUILD
        [ "$tree" = after ] || build=$TREES/$BEFORE/build/release
        for pes in $PES_PER_NODE; do
            for keep in $KEEPS; do
                KIT_BUILD=$build KIT_BIN=$build/kit KIT_SALLOC="$KIT_SALLOC $one" \
                    launch 1 "$pes" "$OUT/claims-$tree-keep$keep-pes$pes-r$round.txt" \
                    SHMEM_SYMMETRIC_SIZE=16m -- "$build/kit/claims" "$ITERATIONS" "$keep"
            done
        done
    done
done
echo "# done $(date -u +%H:%M:%S)" >> "$LOG"
