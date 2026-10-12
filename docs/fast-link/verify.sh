#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The whole verification on the cluster, as round nine's ran in the development
# VM (docs/design/evidence/v0.2-step4/2026-10-11-round-nine-fixes/
# verify-fixes.sh), split by part: one Slurm batch job a part, each on a node
# of the list, whole (--exclusive), so that squeue shows it and Slurm books it
# as it books the kit's own jobs. A part's job copies the tree to
# ~/verify-tree-PART on its node, leaving the kit's own build alone, and writes
# OUTDIR/PART/ there, on the node's own disk: verify.txt each step's verdicts,
# the full outputs beside it. More parts than nodes wrap round the list, and the
# jobs on one node run one after another. The cluster's first session ran the
# parts by hand (docs/design/evidence/v0.2-step4/2026-10-12-fast-link/
# cluster-verify.sh); B12 and B13 of docs/prompts/v0.2-open-items.md made them a
# section of the kit.
#
# The parts: static (the release build's static checks alone); debug and asan
# (each preset's suite at 0, 2 and 3 helpers); release (the static checks, then
# the suite without the models at 0, 2 and 3); tsan (and its gate after each
# helper count); tier3-single (tier 3, then the single-node build); models
# (every model, several at once), or models-K-N (every Nth from the Kth, to
# spread them over N nodes); pacer (at 0 and 2 helpers), or pacer-H (at H
# alone). The default is debug release asan tsan tier3-single models pacer-0
# pacer-2, a node each on eight.
#
#   VERIFY_TIME        each job's time limit in minutes (default 120)
#   VERIFY_JOBS        the tests ctest runs at once: one a core (default the
#                      node's cores, not its CPUs)
#   VERIFY_MODEL_JOBS  the models at once: half the node's CPUs, and no more
#                      than one for each whole 8 GiB of its memory (default) -
#                      one at a time in the development VM's 8 GiB
#
# usage: verify.sh NODES_FILE OUTDIR [PART...]   on the head node: submits
#        verify.sh --part PART OUTDIR            in a part's job, on its node
# OUTDIR is absolute and names a directory on the nodes, such as ~/verify-out.
set -u
KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPOSITORY=$(cd "$KIT/../.." && pwd)

if [ "${1:-}" != --part ]; then
    [ $# -ge 2 ] || { sed -n '/^# usage/,/^# OUTDIR/p' "$0" >&2; exit 2; }
    NODES_FILE=$1; OUTDIR=$2; shift 2
    [ -r "$NODES_FILE" ] || { echo "no node list $NODES_FILE" >&2; exit 2; }
    case $OUTDIR in /*) ;; *) echo "OUTDIR must be absolute: $OUTDIR" >&2; exit 2 ;; esac
    mapfile -t NODES < <(grep . "$NODES_FILE")
    [ ${#NODES[@]} -gt 0 ] || { echo "no node in $NODES_FILE" >&2; exit 2; }
    PARTS=("$@")
    [ ${#PARTS[@]} -gt 0 ] || PARTS=(debug release asan tsan tier3-single models pacer-0 pacer-2)
    mkdir -p "$OUTDIR"
    i=0
    for part in "${PARTS[@]}"; do
        name=${NODES[i % ${#NODES[@]}]}
        # Slurm opens the job's own output before the script runs, so it goes
        # to the home directory, which every node has; the part's go to OUTDIR.
        id=$(sbatch --parsable -J "osoe-verify-$part" -N1 -n1 -w "$name" --exclusive --mem=0 \
                 -t "${VERIFY_TIME:-120}" -o "$HOME/osoe-verify-%j.out" \
                 --wrap "bash '$KIT/verify.sh' --part '$part' '$OUTDIR'") || exit 1
        echo "$part on $name: job $id" | tee -a "$OUTDIR/submitted.txt"
        i=$((i + 1))
    done
    exit 0
fi

[ $# -eq 3 ] || { echo "usage: verify.sh --part PART OUTDIR" >&2; exit 2; }
PART=$2; D=$3/$PART
mkdir -p "$D"; OUT=$D/verify.txt; : > "$OUT"
cores=$(lscpu -p=core,socket 2>/dev/null | grep -v '^#' | sort -u | grep -c .)
[ "$cores" -gt 0 ] || cores=$(nproc)
JOBS=${VERIFY_JOBS:-$cores}
memory_gib=$(awk '/^MemTotal/ { print int($2 / 1048576) }' /proc/meminfo)
MODEL_JOBS=${VERIFY_MODEL_JOBS:-$(( $(nproc) / 2 < memory_gib / 8 ? $(nproc) / 2 : memory_gib / 8 ))}
[ "$MODEL_JOBS" -gt 0 ] || MODEL_JOBS=1
T=$HOME/verify-tree-$PART
rm -rf "${T:?}"; mkdir -p "$T"
rsync -a --exclude=/build --exclude=/.git "$REPOSITORY/" "$T/" || { echo "copy failed" >> "$OUT"; exit 1; }
cd "$T" || exit 1
{
    echo "# $(hostname), $(nproc) CPUs, $cores cores, $memory_gib GiB, job ${SLURM_JOB_ID:-none}," \
         "started $(date -u +%FT%TZ)"
    echo "# kit ${KIT_COMMIT:-$(git -C "$REPOSITORY" rev-parse HEAD 2>/dev/null || echo unknown)}," \
         "tests $JOBS at once, models $MODEL_JOBS"
} >> "$OUT"
suite() {   # preset, environment, extra ctest arguments
    p=$1; e=$2; shift 2
    tag=$p-$(echo "$e" | tr ',=' '__')
    echo "== $p $e $(date +%H:%M:%S)" >> "$OUT"
    env $(echo "$e" | tr ',' ' ') ctest --test-dir build/$p -j"$JOBS" --timeout 600 --output-on-failure "$@" > "$D/$tag.full" 2>&1
    grep -E "tests passed|\*\*\*Failed|\*\*\*Timeout|\*\*\*Skipped|Not Run|\(Failed\)|\(Timeout\)|\(Skipped\)" "$D/$tag.full" >> "$OUT"
}
build() {   # preset
    echo "== build $1 $(date +%H:%M:%S)" >> "$OUT"
    cmake --preset "$1" -DOSOE_TEST_PINS=ON > "$D/configure-$1.txt" 2>&1 || { echo "configure failed $1" >> "$OUT"; return 1; }
    if ! cmake --build "build/$1" -j"$(nproc)" > "$D/build-$1.txt" 2>&1; then
        echo "BUILD FAILED $1" >> "$OUT"; grep -E "error" "$D/build-$1.txt" | head -5 >> "$OUT"; return 1
    fi
    echo "warnings: $(grep -c 'warning:' "$D/build-$1.txt")" >> "$OUT"
}
static() {
    echo "== static checks $(date +%H:%M:%S)" >> "$OUT"
    ctest --test-dir build/release -j"$JOBS" --output-on-failure \
        -R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|license-lines|single-node\.configure|version\.)' \
        > "$D/static.full" 2>&1
    grep -E "tests passed|\(Failed\)" "$D/static.full" >> "$OUT"
}
case $PART in
static)
    build release && static ;;
debug|asan)
    build "$PART" && for h in 0 2 3; do suite "$PART" OSOE_PROGRESS_HELPERS=$h; done ;;
release)
    build release || exit 1
    static
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
    cmake --preset tier3 > "$D/configure-tier3.txt" 2>&1 && cmake --build build/tier3 -j"$(nproc)" > "$D/build-tier3.txt" 2>&1
    ctest --preset tier3 --test-dir build/tier3 --timeout 1500 --output-on-failure > "$D/tier3.full" 2>&1
    grep -E "tests passed|\*\*\*Failed|\*\*\*Timeout" "$D/tier3.full" >> "$OUT"
    echo "== the single-node build $(date +%H:%M:%S)" >> "$OUT"
    cmake -S . -B build/single-node -DCMAKE_BUILD_TYPE=Debug -DOSOE_SINGLE_NODE=ON > "$D/configure-single-node.txt" 2>&1 \
        && cmake --build build/single-node -j"$(nproc)" > "$D/build-single-node.txt" 2>&1 \
        || echo "BUILD FAILED single-node" >> "$OUT"
    echo "warnings: $(grep -c 'warning:' "$D/build-single-node.txt")" >> "$OUT"
    ctest --test-dir build/single-node -j"$JOBS" -LE '^model$' --timeout 600 --output-on-failure > "$D/single-node.full" 2>&1
    grep -E "tests passed|\(Failed\)|\(Timeout\)|\(Skipped\)" "$D/single-node.full" >> "$OUT" ;;
models|models-*)
    build release || exit 1
    mapfile -t M < <(ctest --test-dir build/release -N -L '^model$' | sed -n 's/^ *Test *#[0-9]*: //p' | sort)
    start=0; stride=1
    if [ "$PART" != models ]; then start=$(echo "$PART" | cut -d- -f2); stride=$(echo "$PART" | cut -d- -f3); fi
    names=(); for ((i = start; i < ${#M[@]}; i += stride)); do names+=("${M[i]}"); done
    regex="^($(IFS='|'; echo "${names[*]}" | sed 's/\./\\./g'))\$"
    echo "== models: ${#names[@]} of ${#M[@]}, $MODEL_JOBS at once $(date +%H:%M:%S)" >> "$OUT"
    ctest --test-dir build/release -j"$MODEL_JOBS" -R "$regex" --timeout 3600 --output-on-failure > "$D/models.full" 2>&1
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
