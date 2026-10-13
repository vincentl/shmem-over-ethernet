#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §5, H26 (B11): the cluster's Slurm switched to the binding
# most sites run, and back. `site` writes TaskPlugin=task/affinity,task/cgroup
# and ProctrackType=proctrack/cgroup into every node's slurm.conf, and a
# cgroup.conf constraining cores, so that a step's tasks are bound to the cores
# Slurm allotted them - each PE to its core, where a helper shares its PE's
# core unless the job reserves one (--cpus-per-task, CoreSpec). `none` writes
# back what slurm-setup.sh writes, TaskPlugin=task/none and
# ProctrackType=proctrack/linuxproc, and removes cgroup.conf, for the rest of
# the kit: under task/affinity a job of one task is confined to one core
# whatever --cpu-bind=none says (round nine's review, R1). Run on the
# controller after slurm-setup.sh, with its NODES and CONTROLLER; it restarts
# slurmctld and every slurmd, and ends with a job's two tasks' allowed CPUs,
# the binding's proof - one core each under `site`, every CPU under `none`.
# A node that is this machine is reached without ssh, so a head node on the
# list, or a trial on one machine, needs no ssh to itself.
# usage: [CONTROLLER=HOST] binding.sh NODES site|none
set -eu
NODES_FILE=$1
MODE=${2:-}
FIRST=$(sed -n 1p "$NODES_FILE")
CONTROLLER=${CONTROLLER:-$FIRST}
case $MODE in
site) TASK=task/affinity,task/cgroup; PROCTRACK=proctrack/cgroup ;;
none) TASK=task/none; PROCTRACK=proctrack/linuxproc ;;
*) echo "usage: [CONTROLLER=HOST] binding.sh NODES site|none" >&2; exit 2 ;;
esac
SSH="ssh -o StrictHostKeyChecking=accept-new"
HERE=$(hostname -s)
on() { local n=$1; shift; if [ "${n%%.*}" = "$HERE" ]; then bash -c "$*"; else $SSH "$n" "$*"; fi; }
TARGETS=$(grep . "$NODES_FILE"; grep -qxF "$CONTROLLER" "$NODES_FILE" || echo "$CONTROLLER")
for n in $TARGETS; do
    on "$n" "sudo sed -i -e 's|^TaskPlugin=.*|TaskPlugin=$TASK|' -e 's|^ProctrackType=.*|ProctrackType=$PROCTRACK|' /etc/slurm/slurm.conf &&
             grep -qx 'TaskPlugin=$TASK' /etc/slurm/slurm.conf && grep -qx 'ProctrackType=$PROCTRACK' /etc/slurm/slurm.conf"
    if [ "$MODE" = site ]; then
        on "$n" "printf 'CgroupPlugin=autodetect\nConstrainCores=yes\n' | sudo tee /etc/slurm/cgroup.conf > /dev/null"
    else
        on "$n" "sudo rm -f /etc/slurm/cgroup.conf"
    fi
done
on "$CONTROLLER" "sudo systemctl restart slurmctld"
for n in $(grep . "$NODES_FILE"); do on "$n" "sudo systemctl restart slurmd"; done
sleep 5
sinfo -N -l
srun -N1 -n2 bash -c 'echo "$(hostname) task $SLURM_PROCID: $(grep Cpus_allowed_list /proc/self/status)"'
