#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# slurm-setup.sh's controller, with ssh, sudo, sinfo, srun and sleep stubbed so
# that nothing is changed: each remote command is logged by host, and slurmd -C
# answers with a node line. Two cases - the controller the list's first node,
# and CONTROLLER a head node off the list - each judged by where munge's key,
# slurm.conf, slurmd -C, slurmctld and slurmd went. Run on Linux (base64 -w0);
# stdin is /dev/null, so that a stubbed ssh reads only what a pipe gives it.
# usage: head-node-test.sh SLURM_SETUP
set -u
SCRIPT=$1
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir "$T/bin"
cat > "$T/bin/ssh" <<'STUB'
#!/bin/bash
host=$3; shift 3
case "$*" in
    *"slurmd -C"*) echo "$host: slurmd -C" >> "$LOG"
                   echo "NodeName=$host CPUs=16 Boards=1 SocketsPerBoard=1 CoresPerSocket=8 ThreadsPerCore=2 RealMemory=61000"
                   echo "UpTime=0-00:01:00" ;;
    *) cat > /dev/null; echo "$host: $(printf '%s' "$*" | tr '\n' ' ')" >> "$LOG" ;;   # one line a command
esac
STUB
cat > "$T/bin/sudo" <<'STUB'
#!/bin/bash
case "$1" in test) exit 0 ;; base64) echo "ZmFrZS1rZXk=" ;; *) echo "local: sudo $*" >> "$LOG" ;; esac
STUB
for c in sinfo srun sleep; do printf '#!/bin/bash\necho "local: %s $*" >> "$LOG"\n' "$c" > "$T/bin/$c"; done
chmod +x "$T/bin/"*
printf 'node-a\nnode-b\n' > "$T/nodes"
failures=0
check() {   # NAME EXPECTED ACTUAL
    if [ "$2" = "$3" ]; then echo "  pass  $1: $3"; else echo "  FAIL  $1: expected '$2', got '$3'"; failures=$((failures + 1)); fi
}
hosts_of() { grep -F -- "$1" "$LOG" | cut -d: -f1 | sort -u | tr '\n' ' ' | sed 's/ $//'; }
run_case() {   # NAME CONTROLLER-or-empty
    export LOG="$T/$1.log"; : > "$LOG"
    PATH="$T/bin:$PATH" CONTROLLER=$2 timeout 60 bash "$SCRIPT" "$T/nodes" < /dev/null > "$T/$1.out" 2>&1
    echo "case $1 (CONTROLLER=${2:-unset}): exit $?"
}
run_case first ""
check "munge's key to" "node-a node-b" "$(hosts_of munge.key.new)"
check "slurm.conf to" "node-a node-b" "$(hosts_of slurm.conf)"
check "slurmd -C on" "node-a node-b" "$(hosts_of 'slurmd -C')"
check "slurmctld on" "node-a" "$(hosts_of 'restart slurmctld')"
check "slurmd on" "node-a node-b" "$(hosts_of 'restart slurmd')"
check "SlurmctldHost" "SlurmctldHost=node-a" "$(grep -o 'SlurmctldHost=[^ ]*' "$T/first.out")"
run_case head "head"
check "munge's key to" "head node-a node-b" "$(hosts_of munge.key.new)"
check "slurm.conf to" "head node-a node-b" "$(hosts_of slurm.conf)"
check "slurmd -C on" "node-a node-b" "$(hosts_of 'slurmd -C')"
check "slurmctld on" "head" "$(hosts_of 'restart slurmctld')"
check "slurmd on" "node-a node-b" "$(hosts_of 'restart slurmd')"
check "SlurmctldHost" "SlurmctldHost=head" "$(grep -o 'SlurmctldHost=[^ ]*' "$T/head.out")"
check "node lines" "NodeName=node-a NodeName=node-b" "$(grep -o '^NodeName=[^ ]*' "$T/head.out" | tr '\n' ' ' | sed 's/ $//')"
echo "failures: $failures"
[ "$failures" -eq 0 ]
