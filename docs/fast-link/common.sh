# SPDX-License-Identifier: LGPL-3.0-or-later
# The kit's shared part (docs/fast-link.md), sourced by each section's script:
# where the build is, how a job of NODES nodes of PES PEs each is started, and
# where the results go.
#
#   KIT_LAUNCHER   srun (a cluster, the default) or osoe-launch (one machine's
#                  logical nodes: the dry run on the development VM)
#   KIT_BUILD      the release build, at the same path on every node
#                  (default: build/release under the repository)
#   KIT_SALLOC     salloc's options beside -J and -N, for each job's allocation
#                  (default: --exclusive, whole nodes); a two-node section adds
#                  the pair's names here (pair, below), since salloc chooses
#                  the nodes
#   KIT_SRUN       srun's options beside -N and --ntasks-per-node, for the
#                  allocation's one step (default: -K1 --cpu-bind=none
#                  --overcommit: no binding, and more PEs than CPUs - 32 PEs a
#                  node of 16 - which a step may have only with --overcommit)
#   KIT_TIMEOUT    seconds a job may take before it is ended, SIGKILL 30 s
#                  after SIGTERM; each section sets its own, a few times its
#                  longest job's length (default 900)
#   KIT_COMMIT     the commit the cluster built, where the tree has no .git
#                  (default: git rev-parse HEAD)
#
# Every job runs with OSOE_NO_PROGRESS_TIMEOUT=60 unless it is set, as the
# README asks while a cluster is new: a stalled wait ends the job and names
# what it waited for, rather than holding every node until KIT_TIMEOUT. And
# with SHMEM_DEBUG=1 unless it is set: each job measures the link at start-up
# and sets every data wire's unsent limit from what it measured, and its
# start-up lines - PE 0's link, each node's limit - are the only record of
# either (round nine's review, S2), which summarize.py prints beside each case.
#
# Every job's command line, environment and exit status go to the section's
# run.txt beside its output, and each output file starts with them.

KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPOSITORY=$(cd "$KIT/../.." && pwd)
KIT_LAUNCHER=${KIT_LAUNCHER:-srun}
KIT_BUILD=${KIT_BUILD:-$REPOSITORY/build/release}
KIT_SALLOC=${KIT_SALLOC:---exclusive}
KIT_SRUN=${KIT_SRUN:--K1 --cpu-bind=none --overcommit}
KIT_TIMEOUT=${KIT_TIMEOUT:-900}
KIT_BIN=$KIT_BUILD/kit
export OSOE_NO_PROGRESS_TIMEOUT=${OSOE_NO_PROGRESS_TIMEOUT:-60}
export SHMEM_DEBUG=${SHMEM_DEBUG:-1}

# kit_start NODES_FILE OUTDIR: the node list, its count, and the section's log.
kit_start() {
    NODES_FILE=$1
    OUT=$2
    [ -r "$NODES_FILE" ] || { echo "no node list $NODES_FILE" >&2; exit 2; }
    NODE_COUNT=$(grep -c . "$NODES_FILE")
    mkdir -p "$OUT"
    LOG=$OUT/run.txt
    {
        echo "# $(date -u +%Y-%m-%dT%H:%M:%SZ) $(basename "$0") on $(hostname)"
        echo "# kit ${KIT_COMMIT:-$(git -C "$REPOSITORY" rev-parse HEAD 2>/dev/null || echo unknown)}"
        echo "# launcher $KIT_LAUNCHER, build $KIT_BUILD, nodes $NODE_COUNT," \
             "OSOE_NO_PROGRESS_TIMEOUT=$OSOE_NO_PROGRESS_TIMEOUT, SHMEM_DEBUG=$SHMEM_DEBUG," \
             "timeout $KIT_TIMEOUT s"
    } >> "$LOG"
}

# node NUMBER: the node list's NUMBER-th name, from 1.
node() { sed -n "${1}p" "$NODES_FILE"; }

# block_map NODES PES: osoe-launch's map of NODES logical nodes of PES PEs each.
block_map() {
    local map= i j
    for ((i = 0; i < $1; i++)); do
        for ((j = 0; j < $2; j++)); do
            map=$map${map:+,}$i
        done
    done
    echo "$map"
}

# launch NODES PES OUTFILE [NAME=VALUE...] -- PROGRAM [ARGUMENTS...]: one job,
# its output to OUTFILE after a header of what ran; its status returned, and
# logged. On a cluster each job is an allocation of its own, whole nodes
# (salloc --exclusive), whose one step is the kit's srun: Slurm refuses a job
# more tasks a node than the node has CPUs, whatever --overcommit says -
# select/cons_tres tests a node's tasks before it looks at the overcommit -
# where a step of an allocation may have them (round nine's review, P9-2). So
# KIT_SRUN goes to the step alone - salloc's -K is --kill-command, not srun's
# --kill-on-bad-exit - and the nodes a section names to salloc (KIT_SALLOC).
# Its input is /dev/null: srun forwards a terminal to the tasks, and under
# timeout's process group a read would stop it. Every job is named osoe-kit,
# so that one ended by the timeout - whose killed salloc may leave its
# allocation behind - is cancelled by name; then osoe-stat --gc removes what it
# left in /dev/shm on every node, itself under a timeout, since a node that is
# down would hold it for ever. The kit is run outside an allocation (not under
# salloc): each job asks for whole nodes of its own, which an allocation around
# the kit would hold.
launch() {
    local nodes=$1 pes=$2 file=$3 settings=()
    shift 3
    while [ $# -gt 0 ] && [ "$1" != -- ]; do settings+=("$1"); shift; done
    shift
    {
        echo "# $(date -u +%H:%M:%S) nodes=$nodes pes_per_node=$pes ${settings[*]}"
        echo "# $*"
    } > "$file"
    local status
    if [ "$KIT_LAUNCHER" = osoe-launch ]; then
        env "${settings[@]}" "$KIT_BUILD/tools/launch/osoe-launch" -n $((nodes * pes)) \
            -m "$(block_map "$nodes" "$pes")" -t "$KIT_TIMEOUT" -- "$@" >> "$file" 2>&1
        status=$?
    else
        # shellcheck disable=SC2086
        env "${settings[@]}" timeout -k 30 "$KIT_TIMEOUT" salloc -J osoe-kit -N"$nodes" $KIT_SALLOC \
            srun $KIT_SRUN -N"$nodes" --ntasks-per-node="$pes" "$@" >> "$file" 2>&1 < /dev/null
        status=$?
        if [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; then
            scancel -u "$(id -un)" -n osoe-kit >> "$file" 2>&1
            timeout -k 10 120 srun -J osoe-kit-gc -N"$NODE_COUNT" --ntasks-per-node=1 \
                "$KIT_BUILD/tools/stat/osoe-stat" --gc >> "$file" 2>&1 < /dev/null
        fi
    fi
    echo "# exit $status" >> "$file"
    echo "$(date -u +%H:%M:%S) exit $status $(basename "$file"): ${settings[*]} $*" >> "$LOG"
    return $status
}

# beside NODES PES OUTFILE [NAME=VALUE...] -- PROGRAM [ARGUMENTS...]: launch's
# job, and BESIDE - a command line, run by bash - once on every node of it,
# BESIDE_DELAY seconds in, its output to BESIDE_FILE: for what a node does
# while a job runs, perf's profile of the progress threads (§13). On a cluster
# the job holds its nodes whole (salloc --exclusive), so BESIDE is a step of
# the job's own allocation, beside the job's step (srun --overlap); on one
# machine it runs here as the job does. The job's status is returned, and
# logged; BESIDE's is its file's.
beside() {
    local nodes=$1 pes=$2 file=$3 settings=()
    shift 3
    while [ $# -gt 0 ] && [ "$1" != -- ]; do settings+=("$1"); shift; done
    shift
    {
        echo "# $(date -u +%H:%M:%S) nodes=$nodes pes_per_node=$pes ${settings[*]}"
        echo "# $*"
        echo "# beside it, ${BESIDE_DELAY}s in, on every node: $BESIDE > $(basename "$BESIDE_FILE")"
    } > "$file"
    local status
    if [ "$KIT_LAUNCHER" = osoe-launch ]; then
        env "${settings[@]}" "$KIT_BUILD/tools/launch/osoe-launch" -n $((nodes * pes)) \
            -m "$(block_map "$nodes" "$pes")" -t "$KIT_TIMEOUT" -- "$@" >> "$file" 2>&1 &
        local job=$!
        sleep "$BESIDE_DELAY"
        timeout -k 30 "$KIT_TIMEOUT" bash -c "$BESIDE" > "$BESIDE_FILE" 2>&1 < /dev/null
        wait "$job"
        status=$?
    else
        # shellcheck disable=SC2086,SC2016
        env "${settings[@]}" BESIDE="$BESIDE" BESIDE_FILE="$BESIDE_FILE" BESIDE_DELAY="$BESIDE_DELAY" \
            timeout -k 30 "$KIT_TIMEOUT" salloc -J osoe-kit -N"$nodes" $KIT_SALLOC bash -c '
                srun '"$KIT_SRUN"' -N"$1" --ntasks-per-node="$2" "${@:3}" &
                job=$!
                sleep "$BESIDE_DELAY"
                srun --overlap -N"$1" --ntasks-per-node=1 bash -c "$BESIDE" > "$BESIDE_FILE" 2>&1
                wait "$job"' bash "$nodes" "$pes" "$@" >> "$file" 2>&1 < /dev/null
        status=$?
        if [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; then
            scancel -u "$(id -un)" -n osoe-kit >> "$file" 2>&1
            timeout -k 10 120 srun -J osoe-kit-gc -N"$NODE_COUNT" --ntasks-per-node=1 \
                "$KIT_BUILD/tools/stat/osoe-stat" --gc >> "$file" 2>&1 < /dev/null
        fi
    fi
    echo "# exit $status" >> "$file"
    echo "$(date -u +%H:%M:%S) exit $status $(basename "$file"): ${settings[*]} $* (beside: $BESIDE)" >> "$LOG"
    return $status
}

# on NODE COMMAND...: a command on one node of the list (by name), through Slurm
# on a cluster - a job of one task, beside the others on the node, not of whole
# nodes - or here on the one machine; ended after KIT_TIMEOUT. Its processes may
# use every core of the node, as a job's do, because slurm-setup.sh's
# TaskPlugin is task/none: under task/affinity, a task of a job given less than
# whole nodes is confined to the CPUs Slurm allocated it - a core, under
# CR_Core - whatever --cpu-bind=none says (round nine's review, R1).
on() {
    local name=$1
    shift
    if [ "$KIT_LAUNCHER" = osoe-launch ]; then
        timeout -k 30 "$KIT_TIMEOUT" "$@"
    else
        timeout -k 30 "$KIT_TIMEOUT" srun -J osoe-kit -N1 -n1 --cpu-bind=none -w "$name" "$@" \
            < /dev/null
    fi
}

# spawn NODE OUTFILE COMMAND...: as on, but in the background, its output to
# OUTFILE, and $! the timeout around it rather than a subshell, so that ending
# $! ends the command on its node: timeout passes the signal to srun, which
# passes it to the task.
spawn() {
    local name=$1 file=$2
    shift 2
    if [ "$KIT_LAUNCHER" = osoe-launch ]; then
        timeout -k 30 "$KIT_TIMEOUT" "$@" > "$file" 2>&1 < /dev/null &
    else
        timeout -k 30 "$KIT_TIMEOUT" srun -J osoe-kit -N1 -n1 --cpu-bind=none -w "$name" "$@" \
            > "$file" 2>&1 < /dev/null &
    fi
}

# pair: salloc's option that pins a two-node section's allocation to the
# list's first two nodes, as the bare stream between them is, for KIT_SALLOC;
# nothing on one machine.
pair() {
    [ "$KIT_LAUNCHER" = osoe-launch ] || echo "-w $(node 1),$(node 2)"
}

# rotate LIST ROUND: LIST's words, started at word ROUND, so that each round
# meets the cases in another order and a drift in the machines falls on no one
# case.
rotate() {
    local words=($1) n start i
    n=${#words[@]}
    start=$(($2 % n))
    for ((i = 0; i < n; i++)); do
        printf '%s ' "${words[$(((start + i * (($2 % 2) ? n - 1 : 1)) % n))]}"
    done
    echo
}
