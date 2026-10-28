#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# H23's development builds in the osoe VM (2026-10-27): the worktree's tar,
# packed on the Mac without AppleDouble files, unpacked over /tmp/h23 (mtimes
# kept, so a rebuild compiles what changed), and one preset built.
#   dev-build.sh TAR PRESET [CTEST_ARGS...]
set -u
TAR=$1
PRESET=$2
shift 2
TREE=/tmp/h23
mkdir -p "$TREE" && tar -xf "$TAR" -C "$TREE" && cd "$TREE" || exit 2
if [ ! -f "build/$PRESET/CMakeCache.txt" ]; then
    cmake --preset "$PRESET" > "/tmp/h23-configure-$PRESET.txt" 2>&1 || { tail -30 "/tmp/h23-configure-$PRESET.txt"; exit 3; }
fi
if ! cmake --build "build/$PRESET" -j4 > "/tmp/h23-build-$PRESET.txt" 2>&1; then
    grep -E 'error|warning' "/tmp/h23-build-$PRESET.txt" | head -40
    exit 4
fi
echo "built $PRESET: $(grep -c 'warning:' "/tmp/h23-build-$PRESET.txt") warnings"
grep 'warning:' "/tmp/h23-build-$PRESET.txt" | head -20
if [ $# -gt 0 ]; then
    ctest --test-dir "build/$PRESET" "$@" > "/tmp/h23-ctest-$PRESET.txt" 2>&1
    grep -E 'tests passed|\(Failed\)|\(Timeout\)|\*\*\*Exception' "/tmp/h23-ctest-$PRESET.txt" | head -40
fi
