# SPDX-License-Identifier: LGPL-3.0-or-later
# The kit's shared part (docs/fast-link.md), sourced by each section's script:
# where the build is, how a job of NODES nodes of PES PEs each is started, and
# where the results go.
#
#   KIT_LAUNCHER   srun (a cluster, the default) or osoe-launch (one machine's
#                  logical nodes: the dry run on the development VM)
#   KIT_BUILD      the release build, at the same path on every node
#                  (default: build/release under the repository)
#   KIT_SRUN       srun's options beside -N and --ntasks-per-node
#                  (default: -K1 --cpu-bind=none)
#   KIT_TIMEOUT    seconds a job may take before it is ended (default 900)
#
# Every job's command line, environment and exit status go to the section's
# run.txt beside its output, and each output file starts with them.

KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPOSITORY=$(cd "$KIT/../.." && pwd)
KIT_LAUNCHER=${KIT_LAUNCHER:-srun}
KIT_BUILD=${KIT_BUILD:-$REPOSITORY/build/release}
KIT_SRUN=${KIT_SRUN:--K1 --cpu-bind=none}
KIT_TIMEOUT=${KIT_TIMEOUT:-900}
KIT_BIN=$KIT_BUILD/kit

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
        echo "# kit $(git -C "$REPOSITORY" rev-parse HEAD 2>/dev/null || echo unknown)"
        echo "# launcher $KIT_LAUNCHER, build $KIT_BUILD, nodes $NODE_COUNT"
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
# logged.
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
        env "${settings[@]}" timeout "$KIT_TIMEOUT" srun $KIT_SRUN -N"$nodes" \
            --ntasks-per-node="$pes" "$@" >> "$file" 2>&1
        status=$?
    fi
    echo "# exit $status" >> "$file"
    echo "$(date -u +%H:%M:%S) exit $status $(basename "$file"): ${settings[*]} $*" >> "$LOG"
    return $status
}

# on NODE COMMAND...: a command on one node of the list (by name), through Slurm
# on a cluster, or here on the one machine.
on() {
    local name=$1
    shift
    if [ "$KIT_LAUNCHER" = osoe-launch ]; then
        "$@"
    else
        srun -N1 -n1 -w "$name" "$@"
    fi
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
