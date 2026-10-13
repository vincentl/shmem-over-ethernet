#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §16: the ladder's two rungs that are code, not settings (H6
# of docs/prompts/v0.2-open-items.md): rung 1, a put fills the room the open
# buffer has (de3dc42, beside 3ee3357 before it), and rung 4, the pass parses
# each provided buffer where the kernel put it (2dcf8d6, beside 49c62d3 before
# it) - Part 0's trees, each with its own release build and the kit's probes
# compiled by its own oshcc, beside the tree the kit is (KIT_BUILD). TREES holds
# them, at the same path on every node: each tree written there by git archive
# from this checkout, which needs its .git, copied to every other node of the
# list as build.sh copies the tree, and built on every node at once, each
# node's log in each tree's build-release.txt; BUILD=0 takes the trees TREES
# already holds, built. Then, between the list's first two nodes, two PEs each,
# every tree in another order each round, ROUNDS times: multistream's put
# stream at 64 KiB and at 1 MiB blocks (the rungs' own measures,
# docs/decisions/2026-10-07-one-thread-profile.md), hol's fetch-add alone and
# behind the stream, and the mailbox. Each case's files carry its tree's
# commit, "main" for KIT_BUILD's.
#   COMMITS ("3ee3357 de3dc42 49c62d3 2dcf8d6"), TREES (build/trees under the
#   repository), BUILD (1), ROUNDS (3), STREAM_SECONDS (10), MAILBOX_SECONDS (20)
# usage: trees.sh NODES OUTDIR
: "${KIT_TIMEOUT:=1800}"
source "$(dirname "$0")/common.sh"
kit_start "$1" "$2"
COMMITS=${COMMITS:-3ee3357 de3dc42 49c62d3 2dcf8d6}
TREES=${TREES:-$REPOSITORY/build/trees}
ROUNDS=${ROUNDS:-3}
SECONDS_EACH=${STREAM_SECONDS:-10}
MAILBOX_SECONDS=${MAILBOX_SECONDS:-20}
PROBES="multistream hol"
# One node's builds of every tree, each in its own directory, its probes from
# this tree's kit (docs/fast-link) by that tree's oshcc; a tree whose build
# fails is removed, so that no case runs a build of another.
build_trees() {
    local c status=0
    for c in $COMMITS; do
        ( cd "$TREES/$c" || exit 1
          { cmake --preset release && cmake --build build/release -j"$(nproc)"; } \
              > build-release.txt 2>&1 || { echo "build failed"; exit 1; }
          mkdir -p build/release/kit
          for p in $PROBES; do
              build/release/tools/wrapper/oshcc -O2 -o "build/release/kit/$p" \
                  "$REPOSITORY/docs/fast-link/$p.c" >> build-release.txt 2>&1 ||
                  { echo "$p did not compile"; exit 1; }
          done
          echo "built, $(grep -c 'warning:' build-release.txt) warnings" ) 2>&1 |
            sed "s/^/$(hostname): $c: /"
        [ "${PIPESTATUS[0]}" -eq 0 ] || { rm -rf "$TREES/$c/build"; status=1; }
    done
    return $status
}
if [ "${BUILD:-1}" = 1 ]; then
    mkdir -p "$TREES"
    for c in $COMMITS; do
        rm -rf "${TREES:?}/$c"
        mkdir -p "$TREES/$c"
        git -C "$REPOSITORY" archive "$c" | tar -x -C "$TREES/$c" ||
            { echo "no tree for $c: git archive needs this checkout's .git" | tee -a "$LOG"; exit 1; }
    done
    if [ "$KIT_LAUNCHER" = osoe-launch ]; then
        build_trees | tee -a "$LOG"
        [ "${PIPESTATUS[0]}" -eq 0 ] || exit 1
    else
        for n in $(grep -vxF "$(hostname -s)" "$NODES_FILE"); do
            ssh "$n" mkdir -p "$TREES" &&
                rsync -a --delete "$TREES"/ "$n:$TREES"/ ||
                { echo "copying the trees to $n failed" | tee -a "$LOG"; exit 1; }
        done
        export -f build_trees
        export COMMITS TREES PROBES REPOSITORY
        timeout -k 30 "$KIT_TIMEOUT" srun -J osoe-kit-build -N"$NODE_COUNT" --ntasks-per-node=1 \
            --cpus-per-task="$(nproc)" bash -c build_trees 2>&1 < /dev/null | tee -a "$LOG"
        [ "${PIPESTATUS[0]}" -eq 0 ] || { echo "a tree's build failed on a node" | tee -a "$LOG"; exit 1; }
    fi
fi
for ((round = 1; round <= ROUNDS; round++)); do
    for tree in $(rotate "$COMMITS main" "$round"); do
        build=$KIT_BUILD
        [ "$tree" = main ] || build=$TREES/$tree/build/release
        KIT_BUILD=$build KIT_BIN=$build/kit KIT_SALLOC="$KIT_SALLOC $(pair)" \
            launch 2 2 "$OUT/put-64k-$tree-r$round.txt" -- \
            "$build/kit/multistream" put 65536 "$SECONDS_EACH"
        KIT_BUILD=$build KIT_BIN=$build/kit KIT_SALLOC="$KIT_SALLOC $(pair)" \
            launch 2 2 "$OUT/put-1m-$tree-r$round.txt" -- \
            "$build/kit/multistream" put 1048576 "$SECONDS_EACH"
        KIT_BUILD=$build KIT_BIN=$build/kit KIT_SALLOC="$KIT_SALLOC $(pair)" \
            launch 2 2 "$OUT/hol-$tree-r$round.txt" -- "$build/kit/hol" "$SECONDS_EACH"
        KIT_BUILD=$build KIT_BIN=$build/kit KIT_SALLOC="$KIT_SALLOC $(pair)" \
            launch 2 2 "$OUT/mailbox-$tree-r$round.txt" -- \
            "$build/tools/pacer/osoe-pacer" --seconds "$MAILBOX_SECONDS" mailbox
    done
done
echo "# done $(date -u +%H:%M:%S)" >> "$LOG"
