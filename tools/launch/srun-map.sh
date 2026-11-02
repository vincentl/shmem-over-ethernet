#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# srun-map.sh MAP [SRUN FLAG]... -- PROGRAM [ARGS]...
#
# One step of PROGRAM under srun, a task a rank, each rank on the node its
# entry of MAP names - design §8.1's logical node map, one node id a rank, as
# 0,0,0,1,2,2 - for the maps srun's own distributions cannot give: an uneven
# one, or one whose nodes are out of the ranks' order (1,1,0,0,2,2). A block
# map is --ntasks-per-node and a cyclic one --distribution=cyclic, which the
# cluster tests' registrations emit themselves (shmem/tests/CMakeLists.txt,
# osoe_cluster_command); this script is for the rest, through
# --distribution=arbitrary and the hostfile it takes, one host a rank
# (SLURM_HOSTFILE).
#
# The hosts are the job allocation's, SLURM_JOB_NODELIST, in the order Slurm
# numbers a step's nodes - sorted, the order `scontrol show hostnames` gives -
# so that node id k of the map is the task's SLURM_NODEID k, as the library
# and the test programs read it. Outside an allocation this script allocates
# the map's nodes itself, with as many tasks a node as the map's fullest node
# has, and runs itself inside: salloc takes the partition among the flags
# (-p, --partition) and nothing else of them, since the flags are srun's.
#
# The test cluster.map.<MAP> checks this placement against Slurm's own
# SLURM_NODEID on every rank.
set -eu
usage()
{
    echo "usage: srun-map.sh MAP [SRUN FLAG]... -- PROGRAM [ARGS]..." >&2
    exit 2
}
[ $# -ge 3 ] || usage
MAP=$1
shift
case $MAP in
    ''|*[!0-9,]*|*,,*|,*|*,) echo "srun-map.sh: '$MAP' is not a node map, as 0,0,1,1" >&2; exit 2 ;;
esac
ids=$(printf '%s\n' "$MAP" | tr , '\n')
count=$(printf '%s\n' "$ids" | wc -l | tr -d ' ')
nodes=$(printf '%s\n' "$ids" | sort -nu | wc -l | tr -d ' ')
# Every id from 0 to nodes - 1, as osoe-launch requires of a map (§8.1).
if [ "$(printf '%s\n' "$ids" | sort -nu | tail -1)" -ne $((nodes - 1)) ]; then
    echo "srun-map.sh: '$MAP' skips a node id; a map uses every id from 0 upward" >&2
    exit 2
fi
# The fullest node's tasks: what each node of the allocation must hold.
fullest=$(printf '%s\n' "$ids" | sort -n | uniq -c | awk '{ if ($1 > m) m = $1 } END { print m }')

# The flags before -- are srun's; salloc gets the partition alone.
partition=
seen_separator=no
for argument in "$@"; do
    [ "$argument" = -- ] && break
    case $argument in
        -p|--partition) partition_next=yes; continue ;;
        -p*|--partition=*) partition=$argument ;;
    esac
    if [ "${partition_next:-no}" = yes ]; then
        partition="-p$argument"
        partition_next=no
    fi
done
for argument in "$@"; do
    [ "$argument" = -- ] && seen_separator=yes
done
[ "$seen_separator" = yes ] || usage

if [ -z "${SLURM_JOB_NODELIST:-}" ]; then
    # Not inside an allocation: make one of the map's nodes and run there.
    exec salloc -Q -N "$nodes" --ntasks-per-node="$fullest" $partition \
         sh "$0" "$MAP" "$@"
fi

hosts=$(scontrol show hostnames "$SLURM_JOB_NODELIST")
allocated=$(printf '%s\n' "$hosts" | wc -l | tr -d ' ')
if [ "$allocated" -lt "$nodes" ]; then
    echo "srun-map.sh: the map '$MAP' names $nodes nodes; the allocation" \
         "$SLURM_JOB_NODELIST has $allocated" >&2
    exit 2
fi
hostfile=$(mktemp "${TMPDIR:-/tmp}/osoe-srun-map.XXXXXX")
trap 'rm -f "$hostfile"' EXIT INT TERM HUP
for id in $ids; do
    printf '%s\n' "$hosts" | sed -n "$((id + 1))p" >> "$hostfile"
done

# srun's flags, then the program: the -- between them dropped, since srun's
# options end at its program anyway.
rotate=$#
while [ "$rotate" -gt 0 ]; do
    argument=$1
    shift
    rotate=$((rotate - 1))
    if [ "$seen_separator" = yes ] && [ "$argument" = -- ]; then
        seen_separator=dropped
        continue
    fi
    set -- "$@" "$argument"
done
# --nodes is refused beside --distribution=arbitrary: the hostfile's hosts are
# the step's nodes. Not exec'd, so that the hostfile goes when the step ends;
# the step's status is this script's.
SLURM_HOSTFILE=$hostfile srun -n "$count" --distribution=arbitrary "$@" && status=0 || status=$?
rm -f "$hostfile"
trap - EXIT
exit "$status"
