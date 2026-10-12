#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The cluster's first session (2026-10-12): round nine's full verification
# (docs/design/evidence/v0.2-step4/2026-10-11-round-nine-fixes/verify-fixes.sh)
# split by part, each part on a node of its own at once - native x86-64,
# c4-standard-16, Debian 13 - in a copy of the tree build.sh put at ~/osoe, so
# that the kit's own build/release is left alone. ctest at -j8, a test a core,
# as -j4 was on the VM's four; builds at -j16. PART is one of: debug, asan,
# release (the static checks, then the preset without the models), tsan (its
# gate after each helper count), tier3-single (tier 3, then the single-node
# build), models-a and models-b (the models by name, alternately, one at a
# time; models-K-N every Nth from the Kth), pacer, or pacer-H at H helpers alone.
# usage: cluster-verify.sh PART OUTDIR
set -u
PART=$1; D=$2/$PART
mkdir -p "$D"; OUT=$D/verify.txt; : > "$OUT"
T=$HOME/verify-tree-$PART
rm -rf "${T:?}"; mkdir -p "$T"
rsync -a --exclude=/build "$HOME/osoe/" "$T/" || { echo "copy failed" >> "$OUT"; exit 1; }
cd "$T" || exit 1
echo "# $(hostname), $(nproc) CPUs, started $(date -u +%FT%TZ)" >> "$OUT"
suite() {   # preset, environment, extra ctest arguments
    p=$1; e=$2; shift 2
    tag=$p-$(echo "$e" | tr ',=' '__')
    echo "== $p $e $(date +%H:%M:%S)" >> "$OUT"
    env $(echo "$e" | tr ',' ' ') ctest --test-dir build/$p -j8 --timeout 600 --output-on-failure "$@" > "$D/$tag.full" 2>&1
    grep -E "tests passed|\*\*\*Failed|\*\*\*Timeout|\*\*\*Skipped|Not Run|\(Failed\)|\(Timeout\)|\(Skipped\)" "$D/$tag.full" >> "$OUT"
}
build() {   # preset
    echo "== build $1 $(date +%H:%M:%S)" >> "$OUT"
    cmake --preset "$1" -DOSOE_TEST_PINS=ON > "$D/configure-$1.txt" 2>&1 || { echo "configure failed $1" >> "$OUT"; return 1; }
    if ! cmake --build "build/$1" -j16 > "$D/build-$1.txt" 2>&1; then
        echo "BUILD FAILED $1" >> "$OUT"; grep -E "error" "$D/build-$1.txt" | head -5 >> "$OUT"; return 1
    fi
    echo "warnings: $(grep -c 'warning:' "$D/build-$1.txt")" >> "$OUT"
}
case $PART in
debug|asan)
    build "$PART" && for h in 0 2 3; do suite "$PART" OSOE_PROGRESS_HELPERS=$h; done ;;
release)
    build release || exit 1
    echo "== static checks $(date +%H:%M:%S)" >> "$OUT"
    ctest --test-dir build/release -j8 --output-on-failure \
        -R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|license-lines|single-node\.configure|version\.)' \
        > "$D/static.full" 2>&1
    grep -E "tests passed|\(Failed\)" "$D/static.full" >> "$OUT"
    for h in 0 2 3; do suite release OSOE_PROGRESS_HELPERS=$h -LE '^model$'; done ;;
tsan)
    build tsan && for h in 0 2 3; do
        suite tsan OSOE_PROGRESS_HELPERS=$h
        python3 -I cmake/thread-sanitizer-gate.py judge build/tsan/thread-sanitizer-reports --ctest ctest \
            --build build/tsan --place build/tsan/osoe_thread_sanitizer_place > "$D/tsan-gate-$h.txt" 2>&1
        grep -E '^  passed [0-9]|report files|GATE' "$D/tsan-gate-$h.txt" >> "$OUT"
        rm -rf "$D/tsan-reports-$h"; cp -r build/tsan/thread-sanitizer-reports "$D/tsan-reports-$h"
    done ;;
tier3-single)
    echo "== tier3 $(date +%H:%M:%S)" >> "$OUT"
    cmake --preset tier3 > "$D/configure-tier3.txt" 2>&1 && cmake --build build/tier3 -j16 > "$D/build-tier3.txt" 2>&1
    ctest --preset tier3 --test-dir build/tier3 --timeout 1500 --output-on-failure > "$D/tier3.full" 2>&1
    grep -E "tests passed|\*\*\*Failed|\*\*\*Timeout" "$D/tier3.full" >> "$OUT"
    echo "== the single-node build $(date +%H:%M:%S)" >> "$OUT"
    cmake -S . -B build/single-node -DCMAKE_BUILD_TYPE=Debug -DOSOE_SINGLE_NODE=ON > "$D/configure-single-node.txt" 2>&1 \
        && cmake --build build/single-node -j16 > "$D/build-single-node.txt" 2>&1 \
        || echo "BUILD FAILED single-node" >> "$OUT"
    echo "warnings: $(grep -c 'warning:' "$D/build-single-node.txt")" >> "$OUT"
    ctest --test-dir build/single-node -j8 -LE '^model$' --timeout 600 --output-on-failure > "$D/single-node.full" 2>&1
    grep -E "tests passed|\(Failed\)|\(Timeout\)|\(Skipped\)" "$D/single-node.full" >> "$OUT" ;;
models-*)
    build release || exit 1
    mapfile -t M < <(ctest --test-dir build/release -N -L '^model$' | sed -n 's/^ *Test *#[0-9]*: //p' | sort)
    case $PART in   # models-a and models-b alternate; models-K-N takes every Nth from the Kth
        models-a) start=0; stride=2 ;;
        models-b) start=1; stride=2 ;;
        *) start=$(echo "$PART" | cut -d- -f2); stride=$(echo "$PART" | cut -d- -f3) ;;
    esac
    names=(); for ((i = start; i < ${#M[@]}; i += stride)); do names+=("${M[i]}"); done
    regex="^($(IFS='|'; echo "${names[*]}" | sed 's/\./\\./g'))\$"
    echo "== models: ${#names[@]} of ${#M[@]} $(date +%H:%M:%S)" >> "$OUT"
    ctest --test-dir build/release -j1 -R "$regex" --timeout 3600 --output-on-failure > "$D/models.full" 2>&1
    grep -E "tests passed|\*\*\*Failed|\*\*\*Timeout|Not Run|\(Failed\)" "$D/models.full" >> "$OUT" ;;
pacer|pacer-*)
    build release || exit 1
    helpers="0 2"; [ "$PART" != pacer ] && helpers=${PART#pacer-}
    L=$T/build/release/tools/launch/osoe-launch
    P=$T/build/release/tools/pacer/osoe-pacer
    PI="OSOE_LINK_HOP_NS=86000 OSOE_LINK_MESSAGE_NS=7000 OSOE_LINK_BYTES_PER_SECOND=116000000"
    pacer() {   # label, helpers, then the launcher and the pacer's arguments
        label=$1; h=$2; shift 2
        echo "== pacer $label at $h helpers $(date +%H:%M:%S)" >> "$OUT"
        env OSOE_PROGRESS_HELPERS=$h "$@" > "$D/pacer-$label-$h.txt" 2>&1
        echo "exit $? $(grep -o 'osoe-pacer failures=[0-9]*' "$D/pacer-$label-$h.txt" | tail -1) $(grep -c 'osoe: deadlock' "$D/pacer-$label-$h.txt") deadlock lines" >> "$OUT"
    }
    for h in $helpers; do
        pacer mix $h $L -n 8 -m 0,0,1,1,2,2,3,3 -t 300 -- $P --seconds 60 mix
        pacer mailbox $h $L -n 8 -m 0,0,1,1,2,2,3,3 -t 300 -- $P --seconds 60 mailbox
        pacer collectives $h env $PI $L -n 6 -m 0,0,1,1,2,2 -t 600 -- $P --seconds 60 collectives
        pacer mix-ten-minutes $h env $PI $L -n 8 -m 0,0,1,1,2,2,3,3 -t 900 -- $P --seconds 600 mix
    done ;;
*)
    echo "unknown part $PART" >> "$OUT"; exit 2 ;;
esac
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
