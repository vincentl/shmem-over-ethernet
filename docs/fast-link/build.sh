#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §1: the tree to every node of the list at this same path,
# without .git or build/, and on every node at once, its release build and the
# kit's probes compiled into build/release/kit with the build's own oshcc - so
# that everything a step runs is at the same path on every node, without a
# shared file system. Run on the first node, or on a head node off the list,
# from the tree. Under
# KIT_LAUNCHER=osoe-launch, one machine: the build here alone. Each node's
# configure and build log lands in its build-release.txt, whose failing lines
# come back here; a build that fails removes the node's build/release whole,
# so that no section runs the last build's library, tools or probes unnoticed,
# and the script's status is the failure; the builds are ended after
# KIT_TIMEOUT.
# usage: build.sh NODES
: "${KIT_TIMEOUT:=1800}"
source "$(dirname "$0")/common.sh"
kit_start "$1" "${OUT:-$REPOSITORY/build/fast-link}"
PROBES="multistream hol get-pieces fan-in startup"
build_here() {
    { cmake --preset release && cmake --build build/release -j"$(nproc)"; } > build-release.txt 2>&1 \
        || { rm -rf build/release
             echo "$(hostname): the build failed:"; grep -E 'error|Error' build-release.txt | head -5
             return 1; }
    mkdir -p build/release/kit
    for p in $PROBES; do
        build/release/tools/wrapper/oshcc -O2 -o "build/release/kit/$p" "docs/fast-link/$p.c" \
            || { rm -rf build/release; echo "$(hostname): $p did not compile"; return 1; }
    done
    echo "$(hostname): built, $(grep -c 'warning:' build-release.txt) warnings"
}
cd "$REPOSITORY" || exit 1
if [ "$KIT_LAUNCHER" = osoe-launch ]; then
    build_here | tee -a "$LOG"
    exit "${PIPESTATUS[0]}"
fi
for n in $(tail -n +2 "$NODES_FILE"); do
    ssh "$n" mkdir -p "$REPOSITORY" && \
        rsync -a --delete --exclude=/.git --exclude=/build "$REPOSITORY"/ "$n:$REPOSITORY"/ \
        || { echo "copying the tree to $n failed" | tee -a "$LOG"; exit 1; }
done
export -f build_here
export PROBES
timeout -k 30 "$KIT_TIMEOUT" srun -J osoe-kit-build -N"$NODE_COUNT" --ntasks-per-node=1 \
    --cpus-per-task="$(nproc)" bash -c "cd '$REPOSITORY' && build_here" 2>&1 < /dev/null |
    tee -a "$LOG"
status=${PIPESTATUS[0]}
[ "$status" -eq 0 ] || echo "the build failed on a node: status $status" | tee -a "$LOG"
exit "$status"
