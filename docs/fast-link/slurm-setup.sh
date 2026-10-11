#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §1: Slurm on the list's nodes, the first its controller,
# from the single-host install that ran in the development VM
# (build/scratch/step3/slurm/install-slurm.sh, 2026-09-23) - untested across
# machines until a cluster runs it. Run on the first node, as a user with sudo
# on every node and ssh from this node to each; slurm-wlm and munge already
# installed (§1). On every node it also sets logind's RemoveIPC=no and the
# user's locked-memory limit unlimited, for slurmd's steps too (§1 says why).
# NODES holds Slurm's names for the nodes - the short host names slurmd -C
# reports - not addresses. Every node's host key is taken on first contact.
# usage: slurm-setup.sh NODES
set -eu
NODES_FILE=$1
FIRST=$(sed -n 1p "$NODES_FILE")
COUNT=$(grep -c . "$NODES_FILE")
[ "$(hostname -s)" = "${FIRST%%.*}" ] || echo "note: running on $(hostname -s), not the list's first node $FIRST"
SSH="ssh -o StrictHostKeyChecking=accept-new"
everywhere() { for n in $(grep . "$NODES_FILE"); do $SSH "$n" "$@"; done; }

# munge: the first node's key, on every node, by its standard input rather
# than its command line, where ps would show it; read once, before the loop,
# since the first node's own copy is rewritten in it.
sudo test -s /etc/munge/munge.key || sudo mungekey --create
KEY=$(sudo base64 -w0 /etc/munge/munge.key)
for n in $(grep . "$NODES_FILE"); do
    echo "$KEY" |
        $SSH "$n" "base64 -d | sudo tee /etc/munge/munge.key.new > /dev/null &&
                   sudo mv /etc/munge/munge.key.new /etc/munge/munge.key &&
                   sudo chown munge:munge /etc/munge/munge.key && sudo chmod 400 /etc/munge/munge.key &&
                   sudo systemctl enable munge && sudo systemctl restart munge"
done

# Each node as slurmd -C sees it, less 512 MiB of memory for the system; by its
# path, since Debian 13 keeps /usr/sbin off a user's PATH, and the setup stops
# unless every node gave its line.
NODE_LINES=$(for n in $(grep . "$NODES_FILE"); do
    $SSH "$n" /usr/sbin/slurmd -C | sed -n 's/^\(NodeName=[^ ]*.*\) RealMemory=\([0-9]*\).*/\1 RealMemory=\2/p' |
        awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^RealMemory=/) { split($i, m, "="); $i = "RealMemory=" (m[2] - 512) } print }'
done)
if [ "$(printf '%s\n' "$NODE_LINES" | grep -c '^NodeName=')" -ne "$COUNT" ]; then
    echo "slurmd -C gave $(printf '%s\n' "$NODE_LINES" | grep -c '^NodeName=') node lines for $COUNT nodes:" >&2
    printf '%s\n' "$NODE_LINES" >&2
    exit 1
fi
CONF=$(cat <<CONF
# The fast-link cluster's Slurm (docs/fast-link/slurm-setup.sh).
ClusterName=fastlink
SlurmctldHost=${FIRST%%.*}
MpiDefault=none
ProctrackType=proctrack/linuxproc
TaskPlugin=task/affinity
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
for n in $(grep . "$NODES_FILE"); do
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

$SSH "$FIRST" "sudo systemctl enable slurmctld && sudo systemctl restart slurmctld"
everywhere "sudo systemctl enable slurmd && sudo systemctl restart slurmd"
sleep 5
sinfo -N -l
srun -N"$(grep -c . "$NODES_FILE")" --ntasks-per-node=1 bash -c 'echo "$(hostname) memlock $(ulimit -l)"'
