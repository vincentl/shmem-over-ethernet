#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# H26's two variants as the chain starts their jobs, in the osoe VM's own
# one-node Slurm (task/affinity, which binds), under the VM's lock: a job of
# two tasks - salloc --exclusive, then the step - bound (-K1 --overcommit), and
# with a core reserved a task (-K1 --overcommit --cpus-per-task=2), each task's
# allowed CPUs; nothing in Slurm's configuration changed. The second check: the
# two variants without --overcommit, which the first found leaves tasks unbound.
# usage: binding-check.sh
set -u
exec 9>/tmp/osoe-vm.lock
flock -w 43200 9 || { echo "no lock"; exit 1; }
echo "# lock taken $(date -u +%FT%TZ)"
scontrol show config | grep -E '^(TaskPlugin|TaskPluginParam|SelectTypeParameters|ProctrackType) '
/usr/sbin/slurmd -C 2>/dev/null | head -1
for srun in "-K1" "-K1 --cpus-per-task=2"; do
    echo "== srun $srun"
    # shellcheck disable=SC2086,SC2016
    timeout 60 salloc -J osoe-kit -N1 --exclusive srun $srun -N1 --ntasks-per-node=2 \
        bash -c 'echo "task $SLURM_PROCID: $(grep Cpus_allowed_list /proc/self/status)"' < /dev/null 2>&1
done
echo "# done $(date -u +%FT%TZ)"
