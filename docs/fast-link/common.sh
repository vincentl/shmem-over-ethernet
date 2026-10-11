# SPDX-License-Identifier: LGPL-3.0-or-later
# The kit's shared part (docs/fast-link.md), sourced by each section's script:
# where the build is, how a job of NODES nodes of PES PEs each is started, and
# where the results go.
#
#   KIT_LAUNCHER   srun (a cluster, the default) or osoe-launch (one machine's
#                  logical nodes: the dry run on the development VM)
#   KIT_BUILD      the release build, at the same path on every node
#                  (default: build/release under the repository)
#   KIT_SRUN       srun's options beside -N and --ntasks-per-node (default:
#                  -K1 --cpu-bind=none --overcommit --exclusive: whole nodes,
#                  no binding, and more PEs than CPUs - 32 PEs a node of 16 -
#                  which srun refuses without --overcommit)
#   KIT_TIMEOUT    seconds a job may take before it is ended, SIGKILL 30 s
#                  after SIGTERM; each section sets its own, a few times its
#                  longest job's length (default 900)
#   KIT_COMMIT     the commit the cluster built, where the tree has no .git
#                  (default: git rev-parse HEAD)
#
# Every job runs with OSOE_NO_PROGRESS_TIMEOUT=60 unless it is set, as the
# README asks while a cluster is new: a stalled wait ends the job and names
# what it waited for, rather than holding every node until KIT_TIMEOUT.
#
# Every job's command line, environment and exit status go to the section's
# run.txt beside its output, and each output file starts with them.

KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPOSITORY=$(cd "$KIT/../.." && pwd)
KIT_LAUNCHER=${KIT_LAUNCHER:-srun}
KIT_BUILD=${KIT_BUILD:-$REPOSITORY/build/release}
KIT_SRUN=${KIT_SRUN:--K1 --cpu-bind=none --overcommit --exclusive}
KIT_TIMEOUT=${KIT_TIMEOUT:-900}
KIT_BIN=$KIT_BUILD/kit
export OSOE_NO_PROGRESS_TIMEOUT=${OSOE_NO_PROGRESS_TIMEOUT:-60}

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
             "OSOE_NO_PROGRESS_TIMEOUT=$OSOE_NO_PROGRESS_TIMEOUT, timeout $KIT_TIMEOUT s"
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
# logged. Its input is /dev/null: srun forwards a terminal to the tasks, and
# under timeout's process group a read would stop it. Every job is named
# osoe-kit, so that one ended by the timeout - whose killed srun may leave its
# allocation behind - is cancelled by name; then osoe-stat --gc removes what it
# left in /dev/shm on every node, itself under a timeout, since a node that is
# down would hold it for ever. The kit is run outside an allocation (not under
# salloc), where the steps it runs at once would wait on each other.
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
        env "${settings[@]}" timeout -k 30 "$KIT_TIMEOUT" srun -J osoe-kit $KIT_SRUN \
            -N"$nodes" --ntasks-per-node="$pes" "$@" >> "$file" 2>&1 < /dev/null
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

# on NODE COMMAND...: a command on one node of the list (by name), through Slurm
# on a cluster, or here on the one machine; unbound, so that its processes may
# use every core of the node, as a job's do; ended after KIT_TIMEOUT.
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

# pair: srun's option that pins a two-node section to the list's first two
# nodes, as the bare stream between them is; nothing on one machine.
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
