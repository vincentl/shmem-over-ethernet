# SPDX-License-Identifier: LGPL-3.0-or-later
#
# T44: what launches.sh and probes.sh share. Sourced, not run; needs bash.
#
# The caller sets TREE (an osoe tree built with the release preset), OUT (where
# the program built by build.sh is, and where the outputs go) and HERE (this
# directory). A job runs in the osoe VM, under its lock, by `limactl shell`.

PROGRAM=$OUT/mpi-and-shmem
BASELINE=$OUT/shmem-without-mpi
WRAPPER=$HERE/open-mpi-bootstrap.sh
LAUNCH=$TREE/build/release/tools/launch/osoe-launch
STAT=$TREE/build/release/tools/stat/osoe-stat

# A modest heap: the default 256 MiB a PE is address space the jobs here do not need.
export SHMEM_SYMMETRIC_SIZE=${SHMEM_SYMMETRIC_SIZE:-64m}

# The names in /dev/shm that were not there when the script began: a job's
# leftovers, osoe's own (osoe.*), which osoe-launch removes and mpirun does not,
# and Open MPI's (sm_segment.*).
shm_before=$(ls /dev/shm 2>/dev/null | grep -E '^(osoe|sm_segment)\.' | sort)
new_names() {
    ls /dev/shm 2>/dev/null | grep -E '^(osoe|sm_segment)\.' | sort | comm -13 <(echo "$shm_before") - |
        tr '\n' ' '
}

# Processes of ours left running after a job: by pid, never by name.
strays() {
    ps -eo pid,ppid,etimes,args | grep -E '[m]pi-and-shmem|[m]pirun|[p]rterun|[p]rted|[p]rte ' || true
}

now_ms() {
    date +%s%3N
}

# report_case NAME PES STATUS START: say in one line how a case ended: the exit
# status, the time since START (now_ms then), how many PEs said their check
# passed, the names it left in /dev/shm and the processes left running. The names
# are removed, and the processes killed, by pid.
report_case() {
    local name=$1 pes=$2 status=$3 start=$4 elapsed ok left stray
    elapsed=$(($(now_ms) - start))
    ok=$(grep -c ': check: [0-9]* rounds, ok' "$OUT/$name.txt")
    left=$(new_names)
    printf '%-38s exit %3d %4d.%d s  %d of %d PEs ok%s\n' "$name" "$status" $((elapsed / 1000)) \
        $((elapsed % 1000 / 100)) "$ok" "$pes" "${left:+; left in /dev/shm: $left}" |
        tee -a "$OUT/${SUMMARY:-summary}.txt"
    if [ -n "$left" ]; then
        # shellcheck disable=SC2086
        (cd /dev/shm && rm -f $left)
    fi
    stray=$(strays)
    if [ -n "$stray" ]; then
        echo "  still running after $name:" | tee -a "$OUT/${SUMMARY:-summary}.txt"
        echo "$stray" | sed 's/^/    /' | tee -a "$OUT/${SUMMARY:-summary}.txt"
        echo "$stray" | awk '{ print $1 }' | xargs -r kill 2>/dev/null
        sleep 1
        echo "$stray" | awk '{ print $1 }' | xargs -r kill -9 2>/dev/null
    fi
}

# case_run NAME PES COMMAND...: run COMMAND for at most $LIMIT seconds (90), keep
# its output in $OUT/NAME.txt, and report how it ended.
case_run() {
    local name=$1 pes=$2 start status
    shift 2
    start=$(now_ms)
    timeout -k 5 "${LIMIT:-90}" "$@" > "$OUT/$name.txt" 2>&1
    status=$?
    report_case "$name" "$pes" "$status" "$start"
    return 0
}

# case_signalled NAME PES SIGNAL AFTER COMMAND...: the same, but COMMAND is sent
# SIGNAL after AFTER seconds - by a user at the terminal, or a scheduler - and
# the case ends when it does.
case_signalled() {
    local name=$1 pes=$2 signal=$3 after=$4 start status job
    shift 4
    start=$(now_ms)
    "$@" > "$OUT/$name.txt" 2>&1 &
    job=$!
    sleep "$after"
    kill -"$signal" $job 2>/dev/null
    wait $job
    status=$?
    report_case "$name" "$pes" "$status" "$start"
    return 0
}
