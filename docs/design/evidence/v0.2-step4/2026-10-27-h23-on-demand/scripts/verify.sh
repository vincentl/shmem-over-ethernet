#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# H23's on-demand data wires verified whole in the osoe VM (2026-10-27), after
# batch-verify.sh of 2026-10-24: the tree (a tar of the worktree) built in every
# preset, the single-node build and tier 3's without a warning; the static
# checks first; then release (models apart), debug, asan, tsan with its gate,
# the single-node suite; the internode suite on demand in debug, at no helpers
# and at two, and in asan and tsan; tier 3's suite; and the models named.
# On the VM's disk (/var/tmp), not its tmpfs, which other sessions' trees share.
#   verify.sh TAR NAME [MODELS_REGEX]
set -u
TAR=$1
NAME=$2
MODELS=${3:-}
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=/var/tmp/$NAME
OUT=/var/tmp/$NAME-out
STATUS=$HERE/$NAME.status
SUMMARY=$HERE/$NAME.summary
rm -f "$STATUS"
exec 9>/tmp/osoe-vm.lock
flock -w 10800 9 || { echo "exit 99: the VM's lock was not had in three hours" > "$STATUS"; exit 1; }
say() { echo "$*" | tee -a "$SUMMARY"; }
finish() { say "== $(date -u +%T) $1"; echo "$1" > "$STATUS"; exit 0; }
: > "$SUMMARY"
rm -rf "$TREE" "$OUT" && mkdir -p "$TREE" "$OUT" && tar -xf "$TAR" -C "$TREE" && cd "$TREE" || finish "exit 2: no tree"
say "== $(date -u +%FT%TZ) $NAME from $(basename "$TAR"); load $(cut -d' ' -f1-3 /proc/loadavg)"
free -m | awk '/^Mem:/ { print "memory available: " $7 " MiB" }' | tee -a "$SUMMARY"

for preset in release debug asan tsan tier3; do
    cmake --preset "$preset" > "$OUT/configure-$preset.txt" 2>&1 || finish "exit 3: configure $preset failed"
done
cmake --preset release -DOSOE_SINGLE_NODE=ON -B build/single-node > "$OUT/configure-single-node.txt" 2>&1 ||
    finish "exit 3: configure single-node failed"
for dir in release debug asan tsan tier3 single-node; do
    if ! cmake --build "build/$dir" -j4 > "$OUT/build-$dir.txt" 2>&1; then
        finish "exit 4: build $dir failed"
    fi
    say "built $dir: $(grep -c 'warning:' "$OUT/build-$dir.txt") warnings"
done

STATIC='^(base\.serializing-fence|shmem\.(boundary|exports|license-lines|memory-orders|pass-expressions|profiling|profiling_aliases|settings|settings-documented|settings-known|single-node\.configure|static-link|strict-c11|version|version\.library|whole-lines))$'
ctest --test-dir build/release -R "$STATIC" > "$OUT/static.txt" 2>&1
say "static: $(grep -E 'tests passed' "$OUT/static.txt")"
grep -E '\(Failed\)|\(Timeout\)' "$OUT/static.txt" | tee -a "$SUMMARY"

run() {   # run LABEL DIR [ENV=VALUE...] -- ARGS...: one preset's suite
    local label=$1 dir=$2
    shift 2
    local envs=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do envs+=("$1"); shift; done
    shift
    say "-- $(date -u +%T) $label"
    env "${envs[@]}" ctest --test-dir "build/$dir" -j4 "$@" > "$OUT/$label.txt" 2>&1
    say "$label: $(grep -E 'tests passed' "$OUT/$label.txt")"
    grep -E '\(Failed\)|\(Timeout\)|\*\*\*Exception' "$OUT/$label.txt" | head -12 | tee -a "$SUMMARY"
}
run release release -- -LE '^model$'
run debug debug --
run asan asan --
run tsan tsan --
run single-node single-node -- -LE '^model$'
run on-demand debug OSOE_MESH_DATA_WIRES=on-demand -- -L internode --timeout 300
run on-demand-helpers debug OSOE_MESH_DATA_WIRES=on-demand OSOE_PROGRESS_HELPERS=2 -- -L internode --timeout 300
run on-demand-asan asan OSOE_MESH_DATA_WIRES=on-demand -- -L internode --timeout 600
run on-demand-tsan tsan OSOE_MESH_DATA_WIRES=on-demand -- -L internode --timeout 600
say "-- $(date -u +%T) tier3"
ctest --test-dir build/tier3 -L tier3 -j2 --timeout 600 > "$OUT/tier3.txt" 2>&1
say "tier3: $(grep -E 'tests passed' "$OUT/tier3.txt")"
grep -E '\(Failed\)|\(Timeout\)' "$OUT/tier3.txt" | head -12 | tee -a "$SUMMARY"
if [ -n "$MODELS" ]; then
    say "-- $(date -u +%T) models $MODELS"
    ctest --test-dir build/release -R "$MODELS" --timeout 2400 > "$OUT/models.txt" 2>&1
    say "models: $(grep -E 'tests passed' "$OUT/models.txt")"
    grep -E '\(Failed\)|\(Timeout\)' "$OUT/models.txt" | tee -a "$SUMMARY"
fi
count() { ctest --test-dir "build/$1" -N "${@:2}" | awk '/Total Tests:/ { print $3 }'; }
say "counts: debug $(count debug), of them internode $(count debug -L internode); tsan $(count tsan), internode $(count tsan -L internode); single-node $(count single-node); release without models $(count release -LE '^model$'); tier3 $(count tier3 -L tier3)"
dmesg 2>/dev/null | grep -i -E 'killed process|out of memory' | tail -3 | tee -a "$SUMMARY"
finish "exit 0: done"
