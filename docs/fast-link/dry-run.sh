#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The kit end to end on one machine (docs/fast-link.md): every section's script
# under osoe-launch's logical nodes, at sizes that finish in a few minutes on
# four cores (three in the VM on 2026-10-11, as in round nine's), then
# summarize.py over each section's output. It shows the
# scripts and probes run and their outputs parse - not any number a cluster
# would give. Run from the tree, after `docs/fast-link/build.sh` with
# KIT_LAUNCHER=osoe-launch has built it here.
# SECTIONS names the sections to run, in this order (default: every one).
# usage: dry-run.sh OUTDIR
set -u
export KIT_LAUNCHER=osoe-launch KIT_TIMEOUT=300
# A tree unpacked without .git cannot say its commit: give it as KIT_COMMIT,
# which a checkout's own git rev-parse needs no help with (common.sh).
# A small heap: every logical node's PEs share this machine's /dev/shm, which
# the default heap's 256 MiB a PE would fill at scale.sh's 32 PEs a node.
export SHMEM_SYMMETRIC_SIZE=32m
OUT=$1
KIT=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"
NODES=$OUT/nodes
printf 'one\ntwo\nthree\nfour\nfive\n' > "$NODES"
SECTIONS=${SECTIONS:-facts floor helpers ladder zero-copy gets fan-in scale collect behind window tuning trees}
wanted() { case " $SECTIONS " in *" $1 "*) return 0 ;; esac; return 1; }
run() {   # SECTION [NAME=VALUE...]: the section's script at small sizes
    local section=$1
    shift
    wanted "$section" || return 0
    echo "== $section $(date -u +%H:%M:%S)"
    env "$@" bash "$KIT/$section.sh" "$NODES" "$OUT/$section" > "$OUT/$section.log" 2>&1
    echo "exit $?, jobs that failed: $(grep -c 'exit [1-9]' "$OUT/$section/run.txt" 2>/dev/null)"
}
run facts
run floor CONNECTIONS="1 2" ECHO_MEBIBYTES=64
run helpers ROUNDS=1 HELPERS="0 1 2 4" PES_PER_NODE=2 STREAM_SECONDS=2
run ladder ROUNDS=1 HOL_SECONDS=2
run zero-copy ROUNDS=1 PES_PER_NODE=2 STREAM_SECONDS=2
run gets ROUNDS=1 ZONES="default 262144" LARGEST=4194304 PES_PER_NODE="1 2" STREAM_SECONDS=2
run fan-in ROUNDS=1 PES_PER_NODE="1 2"
run scale PES_PER_NODE=2 MIX_SECONDS=20
run collect ROUNDS=1 PES_PER_NODE=2
run behind ROUNDS=1 STREAM_SECONDS=2 STREAM_PES=2 PROFILE_DELAY=1 PROFILE_SECONDS=1
run window ROUNDS=1 PES_PER_NODE=2 FANOUTS="2 4" WINDOWS="1 4" TRIPS=10
run tuning ROUNDS=1 PES_PER_NODE=2 HOL_SECONDS=2 MAILBOX_SECONDS=5 CONNECTIONS=1 \
    ECHO_MEBIBYTES=16
run trees ROUNDS=1 STREAM_SECONDS=2 MAILBOX_SECONDS=5
for section in floor helpers ladder zero-copy gets fan-in scale collect behind window tuning trees; do
    wanted "$section" || continue
    python3 "$KIT/summarize.py" "$OUT/$section" > "$OUT/$section-summary.txt" 2>&1 \
        || echo "summarize.py failed on $section"
done
echo "DONE $(date -u +%H:%M:%S)"
