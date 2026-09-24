#!/bin/sh
# capture-slurm-fixtures.sh NAME OUTDIR [SRUN ARGS]...
#
# Runs `srun SRUN ARGS` with a task that records its SLURM_ environment, and
# writes one fixture per node of the step to OUTDIR/captured-NAME-node<I>.env,
# for shmem/tests/test-bootstrap.c to replay (design §6.1, §8.2). OUTDIR must be
# on a filesystem every node of the step sees, since each task writes its own
# record there. For example, on a cluster of four nodes:
#
#   capture-slurm-fixtures.sh block-4x4  shmem/tests/slurm-fixtures -N4 --ntasks-per-node=4
#   capture-slurm-fixtures.sh cyclic-4x4 shmem/tests/slurm-fixtures -N4 --ntasks-per-node=4 \
#       --distribution=cyclic
#
# Each fixture's expectation is worked out from every task's own SLURM_PROCID,
# SLURM_NODEID and SLURM_LOCALID - its node's ranks in local-id order - and not
# from SLURM_GTIDS, so replaying it checks §6.1's reading of SLURM_GTIDS against
# what Slurm itself says. The task kept for each node is its local id 1, or 0
# on a node of one task. Names that say who submitted the job or from where are
# dropped.
set -eu
if [ $# -lt 2 ]; then
    echo "usage: capture-slurm-fixtures.sh NAME OUTDIR [SRUN ARGS]..." >&2
    exit 2
fi
NAME=$1 OUTDIR=$2
shift 2
mkdir -p "$OUTDIR"
RAW=$(mktemp -d "$OUTDIR/.capture-$NAME.XXXXXX")
trap 'rm -rf "$RAW"' EXIT

srun "$@" sh -c 'env | grep "^SLURM_" |
    grep -v "^SLURM_JOB_USER=\|^SLURM_JOB_UID=\|^SLURM_JOB_GID=\|^SLURM_SUBMIT_DIR=\|^SLURM_SUBMIT_HOST=" |
    sort > "$0/task-$SLURM_PROCID.env"' "$RAW"

VERSION=$(srun --version 2>/dev/null || echo "slurm, version unknown")
COMMAND="srun $*"
# One line per task: rank node local, then every task's file whole.
for f in "$RAW"/task-*.env; do
    awk -F= '/^SLURM_PROCID=/ { r = $2 } /^SLURM_NODEID=/ { n = $2 }
             /^SLURM_LOCALID=/ { l = $2 } END { print r, n, l }' "$f"
done | sort -n -k2 -k3 > "$RAW/layout"

awk -v raw="$RAW" -v out="$OUTDIR" -v name="$NAME" -v version="$VERSION" \
    -v command="$COMMAND" '
{ rank[NR] = $1; node[NR] = $2; local[NR] = $3; count[$2]++
  list[$2] = (list[$2] == "" ? "" : list[$2] ",") $1 }
END {
    for (i = 1; i <= NR; i++) {
        n = node[i]
        want = count[n] > 1 ? 1 : 0
        if (local[i] != want)
            continue
        file = raw "/task-" rank[i] ".env"
        fixture = out "/captured-" name "-node" n ".env"
        # The values the expectation also needs, from this task alone.
        delete v
        while ((getline line < file) > 0) {
            split(line, kv, "=")
            v[kv[1]] = substr(line, length(kv[1]) + 2)
        }
        close(file)
        print "# captured: " command > fixture
        print "# " version > fixture
        printf "# expect: launcher=SLURM_ rank=%s size=%s node=%s nodes=%s local=%s/%s " \
               "gtids=%s job=%s step=%s hosts=%s\n", rank[i], v["SLURM_STEP_NUM_TASKS"], n,
               v["SLURM_STEP_NUM_NODES"], local[i], count[n], list[n], v["SLURM_JOB_ID"],
               v["SLURM_STEP_ID"], (v["SLURM_STEP_NODELIST"] != "" ? v["SLURM_STEP_NODELIST"] : "-") > fixture
        while ((getline line < file) > 0)
            print line > fixture
        close(file)
        close(fixture)
        print "wrote " fixture
    }
}' "$RAW/layout"
