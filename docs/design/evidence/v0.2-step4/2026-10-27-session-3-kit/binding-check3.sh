#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The third binding check, in the osoe VM's own one-node Slurm under its lock:
# a whole-node allocation's step (salloc --exclusive, then srun, as the kit
# starts a job) of two and of four tasks, with the binding asked for
# (--cpu-bind=cores, and threads) and with two CPUs a task; each task's allowed
# CPUs. Nothing in Slurm's configuration changed.
# usage: binding-check3.sh
set -u
exec 9>/tmp/osoe-vm.lock
flock -w 43200 9 || { echo "no lock"; exit 1; }
echo "# lock taken $(date -u +%FT%TZ)"
for tasks in 2 4; do
    for srun in "-K1 --cpu-bind=cores" "-K1 --cpu-bind=threads" "-K1 --cpu-bind=cores --cpus-per-task=2"; do
        [ "$tasks" = 4 ] && [ "${srun#*cpus-per-task}" != "$srun" ] && continue
        echo "== $tasks tasks, srun $srun"
        # shellcheck disable=SC2086,SC2016
        timeout 60 salloc -J osoe-kit -N1 --exclusive srun $srun -N1 --ntasks-per-node=$tasks \
            bash -c 'echo "task $SLURM_PROCID: $(grep Cpus_allowed_list /proc/self/status)"' < /dev/null 2>&1 |
            grep -v '^salloc:' | sort
    done
done
echo "# done $(date -u +%FT%TZ)"
