#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# T1 and T3's VM part, checks of the 0.2 tag, on the tree to be tagged, in the osoe VM: the four
# presets and the single-node build in its presets, every one without a warning; the static
# checks; each preset's whole suite (the models apart: they run on the host beside this) with
# OSOE_PROGRESS_HELPERS unset - the default, auto - and at 0, 2 and 3 set outright, ThreadSanitizer
# judged by ctest with tsan.reports at each; tier 3's suite; and the release suite under load,
# eight busy loops beside it, at the default. On the VM's disk, the tree removed after.
#   t1.sh TAR NAME
set -u
TAR=$1 NAME=$2
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=/var/tmp/$NAME OUT=/var/tmp/$NAME-out
SUMMARY=$HERE/$NAME.summary STATUS=$HERE/$NAME.status
rm -f "$STATUS"
exec 9>/tmp/osoe-vm.lock
flock -w 10800 9 || { echo "exit 99: no lock" > "$STATUS"; exit 1; }
say() { echo "$*" | tee -a "$SUMMARY"; }
finish() { say "== $(date -u +%T) $1"; echo "$1" > "$STATUS"; exit 0; }
: > "$SUMMARY"
rm -rf "$TREE" "$OUT" && mkdir -p "$TREE" "$OUT" && tar -xf "$TAR" -C "$TREE" && cd "$TREE" || finish "exit 2: no tree"
say "== $(date -u +%FT%TZ) $NAME from $(basename "$TAR"); load $(cut -d' ' -f1-3 /proc/loadavg)"
for preset in release debug asan tsan tier3; do
    cmake --preset "$preset" > "$OUT/configure-$preset.txt" 2>&1 || finish "exit 3: configure $preset"
done
for preset in release debug asan tsan; do
    cmake --preset "$preset" -DOSOE_SINGLE_NODE=ON -B "build/single-node-$preset" > "$OUT/configure-single-node-$preset.txt" 2>&1 ||
        finish "exit 3: configure single-node $preset"
done
for dir in release debug asan tsan tier3 single-node-release single-node-debug single-node-asan single-node-tsan; do
    cmake --build "build/$dir" -j4 > "$OUT/build-$dir.txt" 2>&1 || finish "exit 4: build $dir"
    say "built $dir: $(grep -c 'warning:' "$OUT/build-$dir.txt") warnings"
done
STATIC='^(base\.serializing-fence|shmem\.(boundary|exports|license-lines|memory-orders|pass-expressions|profiling|profiling_aliases|settings|settings-documented|settings-known|single-node\.configure|static-link|strict-c11|version|version\.library|whole-lines))$'
ctest --test-dir build/release -R "$STATIC" > "$OUT/static.txt" 2>&1
say "static: $(grep -E 'tests passed' "$OUT/static.txt")"
run() {   # run LABEL DIR [ENV=VALUE...] -- ARGS...
    local label=$1 dir=$2; shift 2
    local envs=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do envs+=("$1"); shift; done
    shift
    say "-- $(date -u +%T) $label"
    env "${envs[@]}" ctest --test-dir "build/$dir" -j4 -LE '^model$' "$@" > "$OUT/$label.txt" 2>&1
    say "$label: $(grep -E 'tests passed' "$OUT/$label.txt")"
    grep -E '\(Failed\)|\(Timeout\)|\*\*\*Exception' "$OUT/$label.txt" | head -12 | tee -a "$SUMMARY"
}
for helpers in default 0 2 3; do
    env_helpers=(); [ "$helpers" = default ] || env_helpers=(OSOE_PROGRESS_HELPERS=$helpers)
    for preset in release debug asan tsan; do
        run "$preset-helpers-$helpers" "$preset" "${env_helpers[@]}" --
    done
done
for preset in release debug asan tsan; do
    run "single-node-$preset" "single-node-$preset" --
done
say "-- $(date -u +%T) tier3"
ctest --test-dir build/tier3 -L tier3 -j2 --timeout 600 > "$OUT/tier3.txt" 2>&1
say "tier3: $(grep -E 'tests passed' "$OUT/tier3.txt")"
grep -E '\(Failed\)|\(Timeout\)' "$OUT/tier3.txt" | head -12 | tee -a "$SUMMARY"
# Eight busy loops beside the release suite, the default helpers.
pids=(); for i in 1 2 3 4 5 6 7 8; do (while :; do :; done) & pids+=($!); done
run release-under-load release --timeout 600
kill "${pids[@]}" 2>/dev/null; wait 2>/dev/null
count() { ctest --test-dir "build/$1" -N "${@:2}" | awk '/Total Tests:/ { print $3 }'; }
say "counts: release $(count release -LE '^model$'), internode $(count release -L internode); tsan $(count tsan); single-node $(count single-node-release -LE '^model$'); tier3 $(count tier3 -L tier3)"
if [ -n "$(journalctl -k -n 1 --no-pager 2>/dev/null)" ]; then
    say "kernel log: $(journalctl -k --since "-6h" --no-pager 2>/dev/null | grep -i -c -E 'killed process|out of memory') lines naming a memory kill in six hours"
fi
finish "exit 0: done"
