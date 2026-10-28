#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The kit end to end on one machine (docs/fast-link.md): every section's script
# under osoe-launch's logical nodes, at sizes that finish in a few minutes on
# four cores (three in the VM on 2026-10-11, as in round nine's), then
# summarize.py over each section's output. It shows the
# scripts and probes run and their outputs parse - not any number a cluster
# would give. Run from the tree, after `docs/fast-link/build.sh` with
# KIT_LAUNCHER=osoe-launch has built it here.
# SECTIONS names the sections to run, in this order (default: every one), and
# session-3, named alone, the third session's chain whole (session-3.sh) - or
# its sections CHAIN_SECTIONS - at its own settings - rounds, cases, helper
# counts and lengths - on these logical
# nodes, each script's own timeout: H12's 256 PEs a node at WAKE_PES (64), the
# development VM's /dev/shm holding no more; tier 2 the tests of SUITE_TESTS
# (a few, the litmus among them); SOS's sources SOS_SOURCE, sos.sh's (a
# reading corpus's tests-sos: the VM has no git to fetch one); and, on a
# machine without git, as the VM is,
# the four trees of §16 from TREE_TARS, a directory of git archive's tars made
# where git is, COMMIT.tar each, which a stand-in git serves to trees.sh.
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
SECTIONS=${SECTIONS:-facts floor helpers ladder zero-copy gets fan-in scale collect behind window tuning trees claims link-cost}
wanted() { case " $SECTIONS " in *" $1 "*) return 0 ;; esac; return 1; }
if wanted session-3; then
    echo "== session-3 $(date -u +%H:%M:%S)"
    if ! command -v git > /dev/null && [ -n "${TREE_TARS:-}" ]; then
        mkdir -p "$OUT/bin"
        cat > "$OUT/bin/git" <<'GIT'
#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The dry run's stand-in for git: git -C REPOSITORY archive COMMIT, from TREE_TARS.
[ "$1" = -C ] && shift 2
[ "$1" = archive ] && [ -r "$TREE_TARS/$2.tar" ] && exec cat "$TREE_TARS/$2.tar"
echo "the dry run's git serves archive COMMIT from $TREE_TARS alone, not: $*" >&2
exit 1
GIT
        chmod +x "$OUT/bin/git"
        export PATH=$OUT/bin:$PATH TREE_TARS
    fi
    env -u KIT_TIMEOUT SECTIONS="${CHAIN_SECTIONS:-}" WAKE_PES="${WAKE_PES:-64}" SUITE_JOBS="${SUITE_JOBS:-4}" \
        SUITE_TESTS="${SUITE_TESTS:-^shmem[.](quiet[.]litmus[.]L2|wire-pair|barrier-am[.]0-1-2)\$}" \
        bash "$KIT/session-3.sh" "$NODES" "$OUT/session-3" > "$OUT/session-3.log" 2>&1
    echo "exit $?"
    cat "$OUT/session-3/sections.txt"
    echo "DONE $(date -u +%H:%M:%S)"
    exit 0
fi
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
# The kit's build alone (BEFORE empty): the tree before P9 needs git archive.
run claims ROUNDS=1 BEFORE= PES_PER_NODE="2 4" KEEPS="1 2" ITERATIONS=50
run link-cost ROUNDS=1 NODE_COUNTS="2 3" PES_PER_NODE="1 2"
for section in floor helpers ladder zero-copy gets fan-in scale collect behind window tuning trees claims link-cost; do
    wanted "$section" || continue
    python3 "$KIT/summarize.py" "$OUT/$section" > "$OUT/$section-summary.txt" 2>&1 \
        || echo "summarize.py failed on $section"
done
echo "DONE $(date -u +%H:%M:%S)"
