#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §2: what each node is, gathered on every node at once by
# one task a node, into OUTDIR/facts.txt, each line prefixed by its node's name.
# The interface is the one the node's route to the first node uses. The kernel's
# settings are read from /proc/sys - busy polling's two among them, which §15
# sets, and perf's paranoia, which §13's profile meets - and ethtool by its
# path (its -c the interrupt coalescing §15 turns off): Debian 13 keeps
# /usr/sbin off a user's PATH, and sysctl and ethtool are there. A fact that
# cannot be read says why, in its line, rather than going missing.
# usage: facts.sh NODES OUTDIR
: "${KIT_TIMEOUT:=120}"
source "$(dirname "$0")/common.sh"
kit_start "$1" "$2"
FIRST=$(node 1)
# One node's facts, given to every node as bash -c's script, since a file
# written here is on no other node; each line prefixed by its node's name.
ONE_NODE="first=$FIRST
nodes=\"$(grep . "$NODES_FILE" | tr '\n' ' ')\"
$(cat <<'EOF'
say() { sed "s/^/$(hostname) $1: /"; }
uname -a | say kernel
lscpu | say cpu
free -b | say memory
df -B1 /dev/shm | say dev-shm
getconf PAGESIZE | say page-size
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>&1 | say governor
numactl --hardware 2>&1 | say numa
address=$(getent ahostsv4 "$first" | awk 'NR == 1 { print $1 }')
interface=$(ip route get "$address" 2>/dev/null | awk '{ for (i = 1; i < NF; i++) if ($i == "dev") print $(i + 1) }' | head -1)
if [ -z "$interface" ] || [ "$interface" = lo ]; then
    interface=$(ip -o -4 route show default | awk '{ print $5 }' | head -1)
fi
echo "$interface" | say interface
ip -d link show "$interface" | say link
cat "/sys/class/net/$interface/speed" 2>&1 | say speed
for option in -i -k -c -l -g; do /usr/sbin/ethtool $option "$interface" 2>&1 | say "ethtool$option"; done
for key in net.core.netdev_max_backlog net.core.rmem_max net.core.wmem_max net.ipv4.tcp_rmem \
           net.ipv4.tcp_wmem net.ipv4.tcp_congestion_control net.ipv4.tcp_notsent_lowat \
           net.ipv4.tcp_mtu_probing net.ipv4.tcp_slow_start_after_idle net.core.somaxconn \
           net.core.default_qdisc net.core.busy_read net.core.busy_poll \
           kernel.io_uring_disabled kernel.perf_event_paranoid; do
    echo "$key = $(cat "/proc/sys/${key//.//}" 2>&1)" | say sysctl
done
echo "memlock $(ulimit -l), open files $(ulimit -n), stack $(ulimit -s)" | say limits
loginctl show-user "$(id -un)" -p Linger 2>&1 | say linger
grep -h '^ *RemoveIPC' /etc/systemd/logind.conf /etc/systemd/logind.conf.d/*.conf 2>&1 | say logind
dpkg-query -W liburing2 liburing-dev 2>&1 | say packages
for n in $nodes; do echo "$n $(getent ahostsv4 "$n" | awk 'NR == 1 { print $1 }')"; done | say resolves
EOF
)"
{ echo "# SPDX-License-Identifier: LGPL-3.0-or-later"; echo "$ONE_NODE"; } > "$OUT/facts-one-node.sh"
launch "$NODE_COUNT" 1 "$OUT/facts.txt" -- bash -c "$ONE_NODE"
if [ "$KIT_LAUNCHER" = srun ]; then
    scontrol show config > "$OUT/slurm-config.txt" 2>&1
    sinfo -N -l > "$OUT/sinfo.txt" 2>&1
fi
echo "# done $(date -u +%H:%M:%S)" >> "$LOG"
