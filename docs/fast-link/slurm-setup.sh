#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §1: Slurm on the list's nodes, the controller the list's
# first node or CONTROLLER - a head node off the list, which then gets munge's
# key, slurm.conf and slurmctld but no node line, and runs no job -
# from the single-host install that ran in the development VM
# (build/scratch/step3/slurm/install-slurm.sh, 2026-09-23) - untested across
# machines until a cluster runs it. Run on the controller, as a user with sudo
# on every node and ssh from this machine to each; slurm-wlm and munge already
# installed, without their recommended packages (§1). TaskPlugin=task/none, so
# that nothing binds a step's tasks: under task/affinity a job of one task is
# confined to the core Slurm allocated it, whatever --cpu-bind=none says, and
# floor.sh's servers and clients, each such a job, would share one core a node
# (round nine's review, R1). On every node it also sets logind's RemoveIPC=no
# and the user's locked-memory limit unlimited, for slurmd's steps too (§1 says
# why).
# NODES holds Slurm's names for the nodes - the short host names slurmd -C
# reports - not addresses. Every node's host key is taken on first contact.
# usage: [CONTROLLER=HOST] slurm-setup.sh NODES
set -eu
NODES_FILE=$1
FIRST=$(sed -n 1p "$NODES_FILE")
CONTROLLER=${CONTROLLER:-$FIRST}
COUNT=$(grep -c . "$NODES_FILE")
[ "$(hostname -s)" = "${CONTROLLER%%.*}" ] || echo "note: running on $(hostname -s), not the controller $CONTROLLER"
SSH="ssh -o StrictHostKeyChecking=accept-new"
everywhere() { for n in $(grep . "$NODES_FILE"); do $SSH "$n" "$@"; done; }
# What munge's key and slurm.conf go to: the list's nodes, and the controller
# when it is a head node off the list.
TARGETS=$(grep . "$NODES_FILE"; grep -qxF "$CONTROLLER" "$NODES_FILE" || echo "$CONTROLLER")

# munge: this machine's key - the controller's - on every node and on the
# controller, by its standard input rather than its command line, where ps
# would show it; read once, before the loop, since this machine's own copy is
# rewritten in it.
sudo test -s /etc/munge/munge.key || sudo mungekey --create
KEY=$(sudo base64 -w0 /etc/munge/munge.key)
for n in $TARGETS; do
    echo "$KEY" |
        $SSH "$n" "base64 -d | sudo tee /etc/munge/munge.key.new > /dev/null &&
                   sudo mv /etc/munge/munge.key.new /etc/munge/munge.key &&
                   sudo chown munge:munge /etc/munge/munge.key && sudo chmod 400 /etc/munge/munge.key &&
                   sudo systemctl enable munge && sudo systemctl restart munge"
done

# Each node as slurmd -C sees it, less 512 MiB of memory for the system; by its
# path, since Debian 13 keeps /usr/sbin off a user's PATH, and the setup stops
# unless every node gave its line, showing what each that did not printed and
# what to do. slurm-wlm's recommended packages bring the ROCm GPU plugin and
# its library, librocm-smi64; with the library installed, slurmd -C crashed in
# the development VM, a machine without a GPU, with nothing on its output -
# the plugin removed alone, it still did (round nine's review, R2).
NODE_LINES=
MISSING=0
for n in $(grep . "$NODES_FILE"); do
    REPORT=$($SSH "$n" /usr/sbin/slurmd -C 2>&1) && STATUS=0 || STATUS=$?
    LINE=$(printf '%s\n' "$REPORT" | sed -n 's/^\(NodeName=[^ ]*.*\) RealMemory=\([0-9]*\).*/\1 RealMemory=\2/p' |
        awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^RealMemory=/) { split($i, m, "="); $i = "RealMemory=" (m[2] - 512) } print }')
    if [ -z "$LINE" ]; then
        MISSING=$((MISSING + 1))
        echo "slurmd -C on $n gave no node line (status $STATUS); it printed:" >&2
        printf '%s\n' "$REPORT" | sed 's/^/    /' >&2
    fi
    NODE_LINES=$NODE_LINES${NODE_LINES:+$'\n'}$LINE
done
if [ "$MISSING" -gt 0 ]; then
    cat >&2 <<HELP
$MISSING of $COUNT nodes gave no node line, and nothing but munge's key has been
changed. A crash after "Failed to open drm root directory" or "rsmi_init" is
the ROCm SMI library's, on a node without a GPU: on each such node remove it -
sudo apt-get remove -y librocm-smi64-1, which takes the ROCm GPU plugin and
the slurm-wlm-plugins metapackage with it and leaves slurmd; the plugin
removed alone leaves the library, and the crash - or install Slurm as
docs/fast-link.md §1 does, with --no-install-recommends; then run this again.
Otherwise check that /usr/sbin/slurmd is there on that node and that ssh from
here reaches it.
HELP
    exit 1
fi
CONF=$(cat <<CONF
# The fast-link cluster's Slurm (docs/fast-link/slurm-setup.sh).
ClusterName=fastlink
SlurmctldHost=${CONTROLLER%%.*}
MpiDefault=none
ProctrackType=proctrack/linuxproc
# Nothing binds a step's tasks, so that a job of one task may use every core of
# its node (docs/fast-link.md, §1).
TaskPlugin=task/none
SelectType=select/cons_tres
SelectTypeParameters=CR_Core
SchedulerType=sched/backfill
PropagateResourceLimitsExcept=MEMLOCK
InactiveLimit=120
ReturnToService=2
SlurmUser=slurm
SlurmctldPidFile=/run/slurmctld/slurmctld.pid
SlurmdPidFile=/run/slurmd.pid
SlurmdSpoolDir=/var/lib/slurm/slurmd
StateSaveLocation=/var/lib/slurm/slurmctld
SlurmctldLogFile=/var/log/slurm/slurmctld.log
SlurmdLogFile=/var/log/slurm/slurmd.log
$NODE_LINES
PartitionName=main Nodes=ALL Default=YES MaxTime=INFINITE State=UP
CONF
)
echo "$CONF"
ENCODED=$(echo "$CONF" | base64 -w0)
for n in $TARGETS; do
    $SSH "$n" "sudo mkdir -p /var/lib/slurm/slurmctld /var/lib/slurm/slurmd /var/log/slurm &&
              sudo chown -R slurm:slurm /var/lib/slurm /var/log/slurm &&
              echo '$ENCODED' | base64 -d | sudo tee /etc/slurm/slurm.conf > /dev/null"
done

# logind keeps a user's /dev/shm files; the user, and slurmd's steps, lock what
# they need.
everywhere "sudo mkdir -p /etc/systemd/logind.conf.d /etc/security/limits.d /etc/systemd/system/slurmd.service.d &&
            printf '[Login]\nRemoveIPC=no\n' | sudo tee /etc/systemd/logind.conf.d/osoe.conf > /dev/null &&
            printf '* - memlock unlimited\n' | sudo tee /etc/security/limits.d/osoe.conf > /dev/null &&
            printf '[Service]\nLimitMEMLOCK=infinity\n' | sudo tee /etc/systemd/system/slurmd.service.d/memlock.conf > /dev/null &&
            sudo systemctl daemon-reload && sudo systemctl restart systemd-logind"

$SSH "$CONTROLLER" "sudo systemctl enable slurmctld && sudo systemctl restart slurmctld"
everywhere "sudo systemctl enable slurmd && sudo systemctl restart slurmd"
sleep 5
sinfo -N -l
srun -N"$(grep -c . "$NODES_FILE")" --ntasks-per-node=1 bash -c 'echo "$(hostname) memlock $(ulimit -l)"'
