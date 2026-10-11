#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Round nine's kit fixes, in the osoe VM's Slurm (24.11.5, one node of four
# CPUs, CR_Core): the kit's own functions (common.sh) with KIT_LAUNCHER=srun
# and a node list of this one node, from the tree vm-build.sh built.
#   A. Under the VM's own slurm.conf (TaskPlugin=task/affinity, as the kit's
#      slurm-setup.sh had it): P9-2's refusal of the old form; launch's
#      salloc form, 8 tasks on 4 CPUs, and osoe-bench in it; launch's timeout
#      and its clean-up by name, for a job that ends on SIGTERM and one that
#      ignores it; the floor's pinned form; helpers.sh and facts.sh under
#      srun; and R1 as it was - on's and spawn's jobs, and floor.sh's echo part
#      through spawn, 4 connections of ECHO_MEBIBYTES 1024, toward the node's
#      IPv4 address (its name resolves to an IPv6 link-local address first
#      here, and echo-uring's server listens on IPv4 alone).
#   B. Under TaskPlugin=task/none, slurm-setup.sh's setting now: on, spawn and
#      the echo again, launch and osoe-bench. slurm.conf put back by an EXIT
#      trap that only this script runs - the original file, its mtime kept,
#      both daemons restarted, the node resumed if left drained.
#   C. R2: slurm-setup.sh's own loop over the nodes' slurmd -C lines, cut from
#      the script, as installed and with sets of packages hidden in a private
#      mount namespace (their plugins out of a copy of the plugin directory
#      bound over the installed one, their other files under /usr/lib covered
#      by /dev/null), so that nothing outside it changes: Slurm's recommended
#      chain, as --no-install-recommends leaves it out; the ROCm GPU plugin
#      alone; the ROCm SMI library alone. And apt's simulations.
# Under the VM's lock throughout; nothing runs if the queue is not empty.
# usage: vm-slurm-final.sh COMMIT OUTDIR
set -u
COMMIT=$1; OUT=$2
T=/tmp/kit-fixes
S=$T/slurm
rm -rf "$S"
mkdir -p "$OUT" "$S"
exec 9>/tmp/osoe-vm.lock
flock -w 43200 9 || { echo "no lock"; exit 1; }
echo "# lock taken $(date -u +%FT%TZ) on $(hostname), $(srun --version); load $(cut -d' ' -f1-3 /proc/loadavg)"
if squeue -h | grep -q .; then echo "jobs in the queue: Slurm left alone"; squeue; exit 1; fi
cd "$T/src" || exit 1
NODE=$(hostname -s)
ADDRESS=$(getent ahostsv4 "$NODE" | awk 'NR == 1 { print $1 }')
printf '%s\n' "$NODE" > "$S/nodes"
CONF=/etc/slurm/slurm.conf
ORIGINAL=$S/slurm.conf.original
ME=$(id -un)
MAIN=$BASHPID

export KIT_LAUNCHER=srun KIT_BUILD=$T/src/build/release KIT_COMMIT=$COMMIT
# shellcheck disable=SC1091
source docs/fast-link/common.sh
NODES_FILE=$S/nodes
NODE_COUNT=1
LOG=$OUT/kit-run.txt
WHERE='echo "task ${SLURM_PROCID:-?} of ${SLURM_NTASKS:-?} on $(hostname -s): allowed $(grep Cpus_allowed_list /proc/self/status | cut -f2), nproc $(nproc)"'

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
left() {   # what a job may have left: the queue, its steps, and processes
    echo "  queue: [$(squeue -h -u "$ME" -o '%i:%j:%T' | tr '\n' ' ')]" \
         "steps: [$(squeue -h -s -u "$ME" -o '%i:%j' | tr '\n' ' ')]" \
         "processes matching '$1': $(pgrep -u "$ME" -f "$1" | wc -l)"
}
show() { sed 's/^/  | /' "$1"; }
echo_test() {   # NAME: floor.sh's echo part on this node, 4 connections, through spawn
    local name=$1 servers clients guard watch
    spawn "$NODE" "$OUT/$name-servers.txt" bash docs/fast-link/echo-servers.sh \
        "$KIT_BUILD/tools/echo-uring/echo-uring" 4
    servers=$!
    sleep 2
    spawn "$NODE" "$OUT/$name-clients.txt" bash docs/fast-link/echo-clients.sh \
        "$KIT_BUILD/tools/echo-uring/echo-uring" 4 "$ADDRESS" 1024 0
    clients=$!
    ( for i in 1 2 3 4 5 6; do
          sleep 0.5
          for p in $(pgrep -u "$ME" -f 'echo-uring --(server|client)'); do
              echo "$(tr '\0' ' ' < /proc/$p/cmdline 2> /dev/null | grep -o -- '--server\|--client') allowed $(grep Cpus_allowed_list /proc/$p/status 2> /dev/null | cut -f2)"
          done
      done | sort | uniq -c | sed 's/^/  seen: /' > "$OUT/$name-allowed.txt" ) &
    watch=$!
    wait "$clients"
    echo "  clients' status $?"
    wait "$watch"
    cat "$OUT/$name-allowed.txt"
    ( sleep 30; kill "$servers" 2> /dev/null ) &
    guard=$!
    wait "$servers" 2> /dev/null
    kill "$guard" 2> /dev/null
    python3 -c 'import re, statistics, sys
per = {}
for c, r in re.findall(r"^connection=(\d+) uring stream_mib_per_s=([0-9.]+)", open(sys.argv[1]).read(), re.M):
    per.setdefault(c, []).append(float(r))
print(f"  {sys.argv[2]}: the per-connection medians summed {sum(statistics.median(v) for v in per.values()):.0f} MiB/s over {len(per)} connections")' \
        "$OUT/$name-clients.txt" "$name"
    left 'echo-uring'
}

echo
echo "######## A. the VM's own slurm.conf: $(grep -E '^(TaskPlugin|SelectTypeParameters|NodeName)=' "$CONF" | tr '\n' ' ')"
echo "== P9-2: the kit's old form, a job of 8 tasks on 4 CPUs"
srun -J osoe-kit -K1 --cpu-bind=none --overcommit --exclusive -N1 --ntasks-per-node=8 true < /dev/null
echo "  status $?"
wait_idle > /dev/null
echo "== launch: 8 tasks on 4 CPUs"
t0=$(date +%s%N)
launch 1 8 "$OUT/a-launch-where.txt" -- bash -c "$WHERE"
echo "  status $? in $(( ($(date +%s%N) - t0) / 1000000 )) ms"; show "$OUT/a-launch-where.txt"
wait_idle > /dev/null
echo "== launch: osoe-bench quick barrier collectives at 8 PEs on 4 CPUs"
launch 1 8 "$OUT/a-launch-bench.txt" -- "$KIT_BUILD/tools/bench/osoe-bench" quick barrier collectives
echo "  status $?"; grep -E '^(osoe-bench|barrier|alltoall|osoe: PE 0 )' "$OUT/a-launch-bench.txt" | sed 's/^/  | /'
wait_idle > /dev/null
echo "== launch's timeout: KIT_TIMEOUT=5, 8 tasks that would take 60 s"
t0=$(date +%s%N)
KIT_TIMEOUT=5 launch 1 8 "$OUT/a-timeout.txt" -- sleep 60
echo "  status $? in $(( ($(date +%s%N) - t0) / 1000000 )) ms"; show "$OUT/a-timeout.txt"
left 'sleep 60'
wait_idle && echo "  node idle"
echo "== launch's timeout: KIT_TIMEOUT=5, 8 tasks that ignore SIGTERM and would take 120 s"
t0=$(date +%s%N)
KIT_TIMEOUT=5 launch 1 8 "$OUT/a-timeout-term-ignored.txt" -- bash -c 'trap "" TERM; sleep 120'
echo "  status $? in $(( ($(date +%s%N) - t0) / 1000000 )) ms"; show "$OUT/a-timeout-term-ignored.txt"
left 'sleep 120'
wait_idle && echo "  node idle"
echo "== the floor's pinned form: KIT_SALLOC with -w, KIT_SRUN with --cpus-per-task=4"
KIT_SALLOC="$KIT_SALLOC -w $NODE" KIT_SRUN="$KIT_SRUN --cpus-per-task=4" \
    launch 1 1 "$OUT/a-floor-form.txt" -- bash -c "$WHERE"
echo "  status $?"; show "$OUT/a-floor-form.txt"
wait_idle > /dev/null
echo "== R1 as it was: on and spawn, jobs of one task, with --cpu-bind=none"
on "$NODE" bash -c "$WHERE" 2>&1 | sed 's/^/  on | /'
spawn "$NODE" "$OUT/a-spawn.txt" bash -c "$WHERE"
wait $!; echo "  spawn's status $?"; show "$OUT/a-spawn.txt"
echo "== R1 as it was: floor.sh's echo part through spawn, toward $ADDRESS"
echo_test a-echo
wait_idle > /dev/null
echo "== helpers.sh under srun: one round, 0 and 2 helpers, 8 PEs a node on 4 CPUs"
ROUNDS=1 HELPERS="0 2" PES_PER_NODE=8 WORKLOADS="put-1m mailbox collectives" STREAM_SECONDS=2 \
    bash docs/fast-link/helpers.sh "$S/nodes" "$OUT/a-helpers" > "$OUT/a-helpers.log" 2>&1
echo "  status $?"; sed 's/^/  | /' "$OUT/a-helpers/run.txt"
python3 docs/fast-link/summarize.py "$OUT/a-helpers" > "$OUT/a-helpers-summary.txt" 2>&1
echo "  summarize.py: $?"
wait_idle > /dev/null
echo "== facts.sh under srun"
bash docs/fast-link/facts.sh "$S/nodes" "$OUT/a-facts" > "$OUT/a-facts.log" 2>&1
echo "  status $?, $(wc -l < "$OUT/a-facts/facts.txt") lines of facts, scontrol's $(wc -l < "$OUT/a-facts/slurm-config.txt") lines"
sed 's/^/  | /' "$OUT/a-facts/run.txt"

echo
echo "######## B. TaskPlugin=task/none, as slurm-setup.sh now sets it"
wait_idle > /dev/null
if squeue -h | grep -q .; then echo "jobs in the queue: no change made"; squeue; exit 1; fi
sudo cp -p "$CONF" "$ORIGINAL"
ORIGINAL_SUM=$(md5sum < "$CONF")
echo "# original slurm.conf: md5 $ORIGINAL_SUM, $(stat -c '%U:%G %a %s bytes %y' "$CONF")"
restore() {
    # A background subshell holds the traps until it has run its first
    # command, and a kill can come before that: only this script restores.
    [ "$BASHPID" = "$MAIN" ] || exit 0
    trap - EXIT INT TERM HUP
    echo
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
sed 's|^TaskPlugin=.*|TaskPlugin=task/none|' "$ORIGINAL" > "$S/task-none.conf"
sudo cp "$S/task-none.conf" "$CONF"; sudo chown root:root "$CONF"; sudo chmod 644 "$CONF"
sudo systemctl restart slurmctld slurmd
sleep 3
wait_idle
scontrol show config | grep -E "^TaskPlugin " | sed 's/^/  /'
echo "== R1 fixed: on and spawn, jobs of one task"
on "$NODE" bash -c "$WHERE" 2>&1 | sed 's/^/  on | /'
spawn "$NODE" "$OUT/b-spawn.txt" bash -c "$WHERE"
wait $!; echo "  spawn's status $?"; show "$OUT/b-spawn.txt"
echo "== R1 fixed: floor.sh's echo part through spawn, toward $ADDRESS"
echo_test b-echo
wait_idle > /dev/null
echo "== launch under task/none: 8 tasks on 4 CPUs"
launch 1 8 "$OUT/b-launch-where.txt" -- bash -c "$WHERE"
echo "  status $?"; show "$OUT/b-launch-where.txt"
wait_idle > /dev/null
echo "== launch under task/none: osoe-bench quick barrier collectives at 8 PEs"
launch 1 8 "$OUT/b-launch-bench.txt" -- "$KIT_BUILD/tools/bench/osoe-bench" quick barrier collectives
echo "  status $?"; grep -E '^(osoe-bench|barrier|alltoall)|cpu.bind|rror' "$OUT/b-launch-bench.txt" | sed 's/^/  | /'
wait_idle > /dev/null
echo "== P9-2 under task/none: the old form still refused"
srun -J osoe-kit -K1 --cpu-bind=none --overcommit --exclusive -N1 --ntasks-per-node=8 true < /dev/null
echo "  status $?"
restore
trap - EXIT INT TERM HUP

echo
echo "######## C. R2: slurm-setup.sh's loop over the nodes' slurmd -C lines"
PLUGINS=$(dirname "$(dpkg -L slurm-wlm-basic-plugins | grep '/select_cons_tres.so$')")
printf '#!/bin/bash\nshift\nexec "$@"\n' > "$S/local-ssh"; chmod +x "$S/local-ssh"
{
    echo 'set -eu'
    echo "NODES_FILE=$S/nodes; COUNT=1; SSH=$S/local-ssh"
    python3 - docs/fast-link/slurm-setup.sh <<'PYEOF'
import sys
s = open(sys.argv[1]).read()
print(s[s.index("NODE_LINES=\n"):s.index("CONF=$(cat <<CONF")])
PYEOF
    echo 'printf "the node lines:\n%s\n" "$NODE_LINES"'
} > "$S/node-lines.sh"
cat > "$S/in-namespace.sh" <<'NAMESPACE'
# In a private mount namespace: the plugin copy over PLUGINS, /dev/null over
# each library file listed, then slurm-setup.sh's loop.
S=$1; PLUGINS=$2
mount --bind "$S/plugins" "$PLUGINS" || exit 1
while read -r f; do mount --bind /dev/null "$f"; done < "$S/libraries"
bash "$S/node-lines.sh" 2>&1 | grep -v 'Segmentation fault'
echo "  status ${PIPESTATUS[0]}"
NAMESPACE
without() {   # NAME PACKAGE...: the loop as if these packages were not installed
    local name=$1
    shift
    rm -rf "$S/plugins"; mkdir -p "$S/plugins"; cp -a "$PLUGINS"/. "$S/plugins/"
    : > "$S/libraries"
    for p in "$@"; do
        for f in $(dpkg -L "$p"); do
            [ -f "$f" ] && [ ! -L "$f" ] || continue
            case $f in
                "$PLUGINS"/*) rm -f "$S/plugins/$(basename "$f")" ;;
                /usr/lib/*) echo "$f" >> "$S/libraries" ;;
            esac
        done
    done
    echo "== $name: $(ls "$S/plugins" | wc -l) of $(ls "$PLUGINS" | wc -l) plugins, $(wc -l < "$S/libraries") library files hidden"
    sudo unshare -m --propagation private bash "$S/in-namespace.sh" "$S" "$PLUGINS" 2>&1 | sed 's/^/  | /'
}
echo "== as installed, with slurm-wlm's recommended packages"
bash "$S/node-lines.sh" 2>&1 | grep -v 'Segmentation fault' | sed 's/^/  | /'
echo "  status ${PIPESTATUS[0]}"
: > "$S/status"
apt-get install -s -o Dir::State::status="$S/status" slurm-wlm munge 2> /dev/null |
    awk '/^Inst/ { print $2 }' | sort > "$S/with"
apt-get install -s --no-install-recommends -o Dir::State::status="$S/status" slurm-wlm munge 2> /dev/null |
    awk '/^Inst/ { print $2 }' | sort > "$S/without"
echo "== apt's simulation of §1's install from nothing: $(wc -l < "$S/with") packages with the recommended ones, $(wc -l < "$S/without") without"
# From nothing the difference holds base packages too, which every image has:
# Slurm's recommended chain is its plugin packages and the libraries only they
# bring.
CHAIN=$(comm -23 "$S/with" "$S/without" |
    grep -E '^(slurm-wlm-|librocm|liboam|libhdf5|libaec|libsz|freeipmi|libfreeipmi|libipmimonitoring|libmariadb|mariadb-common|mysql-common)' | tr '\n' ' ')
echo "  Slurm's recommended chain: $CHAIN"
# shellcheck disable=SC2086
without "without Slurm's recommended chain, as --no-install-recommends installs it" $CHAIN
without "without the ROCm GPU plugin alone (slurm-wlm-rsmi-plugin)" slurm-wlm-rsmi-plugin
without "without the ROCm SMI library alone (librocm-smi64-1)" librocm-smi64-1
echo "== apt's simulations of the remedies"
echo "  apt-get remove librocm-smi64-1: $(apt-get -s remove librocm-smi64-1 2> /dev/null | awk '/^Remv/ { print $2 }' | tr '\n' ' ')"
echo "  apt-get remove slurm-wlm-rsmi-plugin: $(apt-get -s remove slurm-wlm-rsmi-plugin 2> /dev/null | awk '/^Remv/ { print $2 }' | tr '\n' ' ')"
echo "  outside the namespaces afterwards: $(ls "$PLUGINS" | wc -l) plugin files; $(ls /usr/lib/*/librocm_smi64.so.1.0)"
rm -rf "$S/plugins"
cp "$S/node-lines.sh" "$OUT/"
echo "# done $(date -u +%FT%TZ)"
