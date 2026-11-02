#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# rank-environment.sh RANK:NAME=VALUE... -- PROGRAM [ARGS]...
#
# What osoe-launch -e RANK:NAME=VALUE gives a job on one machine, for a step
# under srun: run as every task of the step, it sets NAME to VALUE in the
# environment of the task whose rank (SLURM_PROCID) is RANK, leaves the other
# tasks' as they are, and runs PROGRAM - so a test whose PEs must disagree
# about a setting runs across real nodes as it does under the launcher:
#
#   srun -N2 --ntasks-per-node=2 rank-environment.sh 1:SHMEM_SYMMETRIC_SIZE=64M -- program
#
# A rank may be named by several assignments, and several ranks each by
# their own.
set -eu
usage()
{
    echo "usage: rank-environment.sh RANK:NAME=VALUE... -- PROGRAM [ARGS]..." >&2
    exit 2
}
if [ -z "${SLURM_PROCID:-}" ]; then
    echo "rank-environment.sh: no SLURM_PROCID; this script runs as a task of a step" >&2
    exit 2
fi
while [ $# -gt 0 ]; do
    case $1 in
        --)
            shift
            break ;;
        *:*=*)
            rank=${1%%:*}
            assignment=${1#*:}
            case $rank in
                ''|*[!0-9]*) usage ;;
            esac
            if [ "$rank" -eq "$SLURM_PROCID" ]; then
                export "$assignment"
            fi
            shift ;;
        *)
            usage ;;
    esac
done
[ $# -gt 0 ] || usage
exec "$@"
