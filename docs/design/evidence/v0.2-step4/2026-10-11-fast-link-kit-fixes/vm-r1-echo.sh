#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Round nine's kit fixes, in the osoe VM's Slurm: R1's size here. floor.sh's
# echo part through the kit's own spawn - servers in one job of one task,
# clients in another - toward the node's IPv4 address, 4 connections each way
# and ECHO_MEBIBYTES 1024 as floor.sh's default; under the VM's
# TaskPlugin=task/affinity and under task/none, alternating, ROUNDS rounds; each
# echo-uring process's allowed CPUs read while it runs. slurm.conf is put back
# by an EXIT trap - byte for byte, both daemons restarted, the node resumed if
# it is left drained - and the background subshells drop the traps they would
# otherwise inherit. Under the VM's lock throughout.
# usage: vm-r1-echo.sh OUTDIR ROUNDS
set -u
OUT=$1; ROUNDS=$2
T=/tmp/kit-fixes
S=$T/slurm
mkdir -p "$OUT" "$S"
exec 9>/tmp/osoe-vm.lock
flock -w 43200 9 || { echo "no lock"; exit 1; }
echo "# lock taken $(date -u +%FT%TZ); load $(cut -d' ' -f1-3 /proc/loadavg)"
if squeue -h | grep -q .; then echo "jobs in the queue: Slurm left alone"; squeue; exit 1; fi
cd "$T/src" || exit 1
NODE=$(hostname -s)
ADDRESS=$(getent ahostsv4 "$NODE" | awk 'NR == 1 { print $1 }')
CONF=/etc/slurm/slurm.conf
ORIGINAL=$S/slurm.conf.original3
ME=$(id -un)
export KIT_LAUNCHER=srun KIT_BUILD=$T/src/build/release
# shellcheck disable=SC1091
source docs/fast-link/common.sh
LOG=$OUT/kit-run.txt

wait_idle() {
    local i s=
    for i in $(seq 1 40); do
        s=$(sinfo -h -N -o %t 2> /dev/null | head -1)
        [ "$s" = idle ] && return 0
        sleep 1
    done
    echo "node state after 40 s: $s"
    return 1
}
use() {   # PLUGIN: slurm.conf's TaskPlugin - the original file itself for the VM's own - both daemons restarted
    if grep -q "^TaskPlugin=$1\$" "$ORIGINAL"; then
        sudo cp -p "$ORIGINAL" "$CONF"
    else
        sed "s|^TaskPlugin=.*|TaskPlugin=$1|" "$ORIGINAL" > "$S/use.conf"
        sudo cp "$S/use.conf" "$CONF"; sudo chown root:root "$CONF"; sudo chmod 644 "$CONF"
    fi
    sudo systemctl restart slurmctld slurmd
    sleep 3
    wait_idle
}
echo_test() {   # NAME: 4 connections each way's worth on this node, through spawn
    local name=$1 servers clients guard watch
    spawn "$NODE" "$OUT/$name-servers.txt" bash docs/fast-link/echo-servers.sh \
        "$KIT_BUILD/tools/echo-uring/echo-uring" 4
    servers=$!
    sleep 2
    spawn "$NODE" "$OUT/$name-clients.txt" bash docs/fast-link/echo-clients.sh \
        "$KIT_BUILD/tools/echo-uring/echo-uring" 4 "$ADDRESS" 1024 0
    clients=$!
    # Each echo-uring process's allowed CPUs, read while the clients run.
    ( trap - EXIT INT TERM HUP
      for i in 1 2 3 4 5 6; do
          sleep 0.5
          for p in $(pgrep -u "$ME" -f 'echo-uring --(server|client)'); do
              echo "$(tr '\0' ' ' < /proc/$p/cmdline 2> /dev/null | grep -o -- '--[a-z]*') $(grep Cpus_allowed_list /proc/$p/status 2> /dev/null | cut -f2)"
          done
      done | sort | uniq -c | sed 's/^/  seen: /' > "$OUT/$name-allowed.txt" ) &
    watch=$!
    wait "$clients"
    echo "  clients' status $?"
    wait "$watch"
    cat "$OUT/$name-allowed.txt"
    ( trap - EXIT INT TERM HUP; sleep 30; kill "$servers" 2> /dev/null ) &
    guard=$!
    wait "$servers"
    kill "$guard" 2> /dev/null
    python3 -c 'import re, statistics, sys
per = {}
for c, r in re.findall(r"^connection=(\d+) uring stream_mib_per_s=([0-9.]+)", open(sys.argv[1]).read(), re.M):
    per.setdefault(c, []).append(float(r))
print(f"  {sys.argv[2]}: the per-connection medians summed {sum(statistics.median(v) for v in per.values()):.0f} MiB/s over {len(per)} connections")' \
        "$OUT/$name-clients.txt" "$name"
    echo "  left: $(squeue -h -u "$ME" | wc -l) jobs, $(pgrep -u "$ME" -f 'echo-uring' | wc -l) echo-uring processes"
}

sudo cp -p "$CONF" "$ORIGINAL"
ORIGINAL_SUM=$(md5sum < "$CONF")
echo "# original slurm.conf: md5 $ORIGINAL_SUM, $(stat -c '%U:%G %a %s bytes %y' "$CONF")"
MAIN=$BASHPID
restore() {
    # A background subshell inherits the traps until it has run its first
    # command, and a kill can come before that: only the script itself restores.
    [ "$BASHPID" = "$MAIN" ] || exit 0
    trap - EXIT INT TERM HUP
    echo "######## restore: the original slurm.conf, both daemons restarted"
    scancel -u "$ME" -n osoe-kit 2> /dev/null
    sudo cp -p "$ORIGINAL" "$CONF"
    sudo systemctl restart slurmctld slurmd
    sleep 3
    if ! wait_idle; then
        sinfo -N -o "%N %t %E"
        echo "resuming the node"
        sudo scontrol update nodename="$NODE" state=resume
        wait_idle
    fi
    sinfo -N -o "%N %t %c %z %E"
    echo "md5 now $(md5sum < "$CONF") (original $ORIGINAL_SUM); $(stat -c '%U:%G %a %s bytes %y' "$CONF")"
    scontrol show config | grep -E "^(SelectTypeParameters|SlurmdParameters|TaskPlugin |InactiveLimit|SlurmctldDebug |DebugFlags)"
    systemctl is-active slurmctld slurmd
    timeout 60 srun -n2 hostname
    echo "restored $(date -u +%FT%TZ)"
}
trap restore EXIT
trap 'exit 130' INT TERM HUP
for ((r = 1; r <= ROUNDS; r++)); do
    for plugin in task/affinity task/none; do
        use "$plugin"
        echo "== round $r, $(scontrol show config | grep -E '^TaskPlugin ' | tr -s ' '), toward $ADDRESS"
        echo_test "r$r-$(basename "$plugin")"
    done
done
