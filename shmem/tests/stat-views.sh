#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# What osoe-stat shows of a job it reads while the job runs or hangs:
#
#   stat-views.sh MODE LAUNCHER STAT SLEEPER INIT
#
# SLEEPER is a program that initializes, says "running N" and sleeps
# (sleeper.c); INIT one that initializes and finishes (test-init.c). Each mode
# starts a job in the background, waits until it is where the mode needs it,
# reads it with STAT, and ends it:
#
#   many-nodes  66 logical nodes of one PE: `osoe-stat JOB`, `--summary JOB`
#               and the listing must each show all 66, where they showed 64 and
#               left the rest unnamed (D2; round four's readiness Q5-5).
#   many-pes    one node of 70 PEs: every row of the counters view must hold a
#               value for each of the 70 and a total, where it held 64 (D3).
#   stopped     two nodes of one PE, node 1's stopped by SIGSTOP: its PE line
#               and the summary must say it is stopped, where it read as a
#               waiting one (D6).
#   init-hang   two nodes of two PEs, rank 3 never calling shmem_init: every PE
#               that has a record - node 0's two and node 1's lead - must show
#               where it waits in shmem_init, where each read "-" (D7).
#   last-pass   two nodes of one PE, node 1's stopped by SIGSTOP for two seconds:
#               its agent's last pass must be that old, and node 0's agent's,
#               which passes ten times a second, under a second; where a look
#               showed passes alone, and only a second look told one standing
#               still (D8).
set -u
MODE=$1 LAUNCHER=$2 STAT=$3 SLEEPER=$4 INIT=$5
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp) || exit 1
job=
cleanup() {
    [ -n "$job" ] && kill -TERM "$job" 2>/dev/null
    [ -n "$job" ] && wait "$job" 2>/dev/null
    [ -n "${stopped_pid:-}" ] && kill -CONT "$stopped_pid" 2>/dev/null
    rm -f "$out"
}
trap cleanup EXIT

# Wait until `count` lines of the job's output match `pattern`, or 60 s pass.
await() {
    tries=0
    while [ "$(grep -c -- "$2" "$out")" -lt "$1" ]; do
        tries=$((tries + 1))
        [ "$tries" -gt 600 ] && { echo "stat-views.sh: waited 60 s for $1 x '$2'"; cat "$out"; exit 1; }
        sleep 0.1
    done
}

fail() { echo "stat-views.sh: $*"; exit 1; }

case $MODE in
many-nodes)
    nodes=66
    map=$(seq -s, 0 $((nodes - 1)))
    OSOE_AM_BUFFER=131072 SHMEM_SYMMETRIC_SIZE=4m \
        "$LAUNCHER" -n $nodes -m "$map" -t 120 -- "$SLEEPER" > "$out" 2>&1 &
    job=$!
    await $nodes '^running '
    shown=$("$STAT" "$job" | grep -c "^/osoe\.$job\.[0-9]*\.[0-9]*\.0  layout")
    summary=$("$STAT" --summary "$job" | grep -c '^node [0-9]')
    listed=$("$STAT" | grep -c "^/osoe\.$job\.")
    note=$("$STAT" | grep -c 'more not shown')
    echo "stat-views.sh: $nodes nodes; osoe-stat JOB showed $shown, --summary $summary" \
         "lines, the listing $listed, $note notes of more not shown"
    [ "$shown" -eq $nodes ] || fail "osoe-stat JOB showed $shown of $nodes nodes"
    # One line a node for its shape and one for its world team, at least.
    [ "$summary" -ge $nodes ] || fail "--summary showed $summary lines for $nodes nodes"
    [ "$listed" -eq $nodes ] && [ "$note" -eq 0 ] || fail "the listing showed $listed of $nodes"
    ;;
many-pes)
    pes=70
    SHMEM_SYMMETRIC_SIZE=4m "$LAUNCHER" -n $pes -t 120 -- "$SLEEPER" > "$out" 2>&1 &
    job=$!
    await $pes '^running '
    view=$("$STAT" -c "$job")
    header=$(printf '%s\n' "$view" | grep -m 1 '^counter ')
    columns=$(printf '%s\n' "$header" | tr -s ' ' '\n' | grep -c '^PE[0-9]')
    # Every counter row: its name, a value a PE, and the total.
    rows=$(printf '%s\n' "$view" | sed -n '/^counter /,$p' | sed 1d |
        awk 'NF > 1 && $1 ~ /\./ { print NF }' | sort -u | tr '\n' ' ')
    echo "stat-views.sh: $pes PEs; the header names $columns, the rows' fields: $rows"
    [ "$columns" -eq $pes ] || fail "the header names $columns PEs of $pes"
    [ "$rows" = "$((pes + 2)) " ] || fail "rows of $rows fields, not $((pes + 2))"
    ;;
stopped)
    "$LAUNCHER" -n 2 -m 0,1 -t 120 -- "$SLEEPER" > "$out" 2>&1 &
    job=$!
    await 2 '^running '
    stopped_pid=$("$STAT" "/osoe.$job.0.1.0" | awk '$1 == "0" && $2 == "1" { print $3; exit }')
    [ -n "$stopped_pid" ] || fail "no PE line for rank 1 in node 1's segment"
    kill -STOP "$stopped_pid"
    pe_line=$("$STAT" "/osoe.$job.0.1.0" | awk '$1 == "0" && $2 == "1"')
    summary=$("$STAT" --summary "$job" | grep '^node 1 ')
    printf 'stat-views.sh: rank 1 (pid %s) stopped; its line: %s\n%s\n' "$stopped_pid" \
        "$pe_line" "$summary"
    printf '%s\n' "$pe_line" | grep -q '\[stopped: SIGSTOP or a debugger\]' ||
        fail "rank 1's line does not say it is stopped"
    printf '%s\n' "$summary" | grep -q '\[1 PE stopped\]' ||
        fail "node 1's summary does not say a PE is stopped"
    ;;
init-hang)
    OSOE_TEST_NOTICE_SECONDS=60 "$LAUNCHER" -n 4 -m 0,0,1,1 -t 120 -k -- \
        sh "$here/exit-before-init.sh" 3 "$INIT" > "$out" 2>&1 &
    job=$!
    await 1 'rank 3 exited'
    sleep 1
    view=$("$STAT" "$job")
    waiting=$(printf '%s\n' "$view" | grep -c 'shmem_init (\(PE\|node\) [0-9]*) for ')
    printf '%s\n' "$view"
    echo "stat-views.sh: $waiting PE lines show where they wait in shmem_init"
    [ "$waiting" -eq 3 ] || fail "$waiting of 3 waiting PEs show where they wait"
    ;;
last-pass)
    "$LAUNCHER" -n 2 -m 0,1 -t 120 -- "$SLEEPER" > "$out" 2>&1 &
    job=$!
    await 2 '^running '
    stopped_pid=$("$STAT" "/osoe.$job.0.1.0" | awk '$1 == "0" && $2 == "1" { print $3; exit }')
    [ -n "$stopped_pid" ] || fail "no PE line for rank 1 in node 1's segment"
    kill -STOP "$stopped_pid"
    sleep 2
    # The agent's row ends with its last pass's age, a number and its unit, or
    # "-": in seconds here, or "none".
    age='$2 == "agent" { if ($NF == "-") { print "none"; exit }
                         split("s ms us ns", unit); split("1 1e-3 1e-6 1e-9", scale)
                         for (i = 1; i <= 4; i++) if ($NF == unit[i]) print $(NF - 1) * scale[i]
                         exit }'
    stuck=$("$STAT" "/osoe.$job.0.1.0" | awk "$age")
    running=$("$STAT" "/osoe.$job.0.0.0" | awk "$age")
    echo "stat-views.sh: node 1 stopped for 2 s; its agent's last pass ${stuck:-?} s ago," \
         "node 0's ${running:-?} s ago"
    awk -v s="${stuck:-none}" 'BEGIN { exit !(s != "none" && s >= 1.5) }' ||
        fail "the stopped node's agent's last pass is ${stuck:-not shown}, not 2 s old"
    awk -v s="${running:-none}" 'BEGIN { exit !(s != "none" && s < 1) }' ||
        fail "the running node's agent's last pass is ${running:-not shown}, not under 1 s"
    ;;
*)
    fail "no mode $MODE"
    ;;
esac
echo "STAT VIEWS OK"
