#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# B10 (docs/prompts/v0.2-open-items.md): does `RemoveIPC=no`, or lingering, keep
# the names a starting job holds in /dev/shm when its user's last login session
# ends? The README said "neither has been tried here".
#
# Run as root in the osoe development VM, from a copy of this directory in
# /tmp/b10-out, as a transient service - it then belongs to no login session,
# and `systemctl stop b10-experiment` ends it, undoing as it goes:
#
#   sudo systemd-run --unit=b10-experiment --collect -p TimeoutStopSec=300 \
#     -p StandardOutput=append:/tmp/b10-out/experiment.log -p StandardError=inherit \
#     bash /tmp/b10-out/experiment.sh run
#
# B10_RUNS names the directory the runs are written to (default /tmp/b10-out)
# and B10_SESSION_CLASS the class of the login sessions (XDG_SESSION_CLASS; su's
# are of class background unless told otherwise), both given to the service
# with `systemd-run -E`.
#
# MODE
#   check    make the account, see that a session of it is one logind counts, undo
#   run      the whole experiment, then undo and verify (writes FINISHED last)
#   running  one more run, of a job past its start (late-pe.c's hold), then the same
#   undo     put everything back from the saved copies; safe to repeat
#   verify   print the state the undo restores, changing nothing
#   cleanup  remove the experiment's files: /tmp/b10, /tmp/b10-out, /var/tmp/b10-saved
#
# What it changes, all in the VM, all undone: an account (osoe-b10, with a
# home); at one point lingering for it; at another `RemoveIPC=no` in
# /etc/systemd/logind.conf, with logind restarted. It takes /tmp/osoe-vm.lock
# first and holds it until it has exited, so that no other session's job runs
# meanwhile.
#
# The runs. Each starts a job of four PEs, rank 1 calling shmem_init 25 s late
# (late-pe.c), so that the start is still going on when logind would remove the
# names, ten seconds after the account's last login session ends. A login
# session is `su -l ACCOUNT -c login-body.sh`: its PAM stack (pam_systemd) opens
# a session that logind lists (login.txt in each run says so).
#   service  the job is started right after the session has ended, outside it,
#            as a transient service of the account - as Slurm's daemon starts a
#            job, which is no child of any login
#   login    the job is started from the session, in the background, and the
#            session ends; the job goes on in the session's scope
#   held     the job runs in the session, which ends when the job does
# The configurations: logind as shipped, lingering enabled, and `RemoveIPC=no`.
set -u

account=osoe-b10
tree=/tmp/b10
out=/tmp/b10-out
runs=${B10_RUNS:-$out}
session_class=${B10_SESSION_CLASS:-}
saved=/var/tmp/b10-saved
lock=/tmp/osoe-vm.lock
late_seconds=25
timeout_seconds=40
hold_seconds=
if [ "$account" != osoe-b10 ]; then exit 9; fi

log() { printf '%s  %s\n' "$(date +%s.%N)" "$*"; }

take_lock() {
    # Opened for reading, not for creation: /tmp's protected_regular refuses root
    # the creating open of a file another user made there, and flock does not
    # need a writable descriptor.
    if ! exec 9< "$lock"; then
        log "cannot open $lock"
        return 1
    fi
    if ! flock -w "$1" 9; then
        log "no lock in $1 s"
        return 1
    fi
    log "holding $lock"
}

# --- logind's state -----------------------------------------------------------

logind_property() {
    busctl get-property org.freedesktop.login1 /org/freedesktop/login1 \
        org.freedesktop.login1.Manager "$1" 2>&1 | awk '{ print $2 }'
}

describe_logind() {
    printf 'RemoveIPC=%s UserStopDelayUSec=%s KillUserProcesses=%s linger_file=%s' \
        "$(logind_property RemoveIPC)" "$(logind_property UserStopDelayUSec)" \
        "$(logind_property KillUserProcesses)" \
        "$([ -e "/var/lib/systemd/linger/$account" ] && echo yes || echo no)"
}

restart_logind() {
    systemctl restart systemd-logind
    local tries=0
    until loginctl list-sessions > /dev/null 2>&1; do
        tries=$((tries + 1))
        if [ "$tries" -gt 60 ]; then break; fi
        sleep 0.5
    done
    log "logind restarted: $(describe_logind)"
}

user_listed() {
    loginctl list-users --no-legend 2> /dev/null \
        | awk -v name="$account" '$2 == name { found = 1 } END { exit !found }'
}

user_state() {
    loginctl list-users --no-legend 2> /dev/null | awk -v name="$account" '$2 == name { print $4 }'
}

wait_for_user_gone() {
    local deadline=$(($(date +%s) + $1))
    while user_listed; do
        if [ "$(date +%s)" -ge "$deadline" ]; then
            log "logind still lists $account, $(user_state)"
            return 1
        fi
        sleep 0.5
    done
}

wait_for_state() {
    local deadline=$(($(date +%s) + $2))
    until [ "$(user_state)" = "$1" ]; do
        if [ "$(date +%s)" -ge "$deadline" ]; then
            log "$account is $(user_state), not $1"
            return 1
        fi
        sleep 0.5
    done
}

set_remove_ipc_no() {
    sed -i 's/^#RemoveIPC=yes$/RemoveIPC=no/' /etc/systemd/logind.conf
    log "logind.conf: $(grep -n '^RemoveIPC' /etc/systemd/logind.conf)"
    restart_logind
    if [ "$(logind_property RemoveIPC)" != false ]; then
        log "RemoveIPC is not false after the restart"
        return 1
    fi
}

restore_logind_conf() {
    cp -a "$saved/logind.conf" /etc/systemd/logind.conf
    if sha256sum -c --quiet "$saved/logind.conf.sha256"; then
        log "logind.conf restored: $(stat -c '%U:%G %a %s bytes, modified %y' /etc/systemd/logind.conf)"
    else
        log "logind.conf DIFFERS from the saved copy"
    fi
    restart_logind
    if [ "$(logind_property RemoveIPC)" != true ]; then
        log "RemoveIPC is not true after the restart"
        return 1
    fi
}

# --- the account and its sessions ---------------------------------------------

# True when the account that exists is the one this run made.
made_by_this_run() {
    [ -s "$saved/account.uid" ] && [ "$(id -u "$account" 2> /dev/null)" = "$(cat "$saved/account.uid")" ]
}

names_of_account() {
    find /dev/shm -maxdepth 1 -user "$account" -name 'osoe.*' -printf '%f\n' 2> /dev/null
}

# Each prints a line - the time and the value - whenever the value changes, and
# runs until killed. The value is the set of the account's names in /dev/shm
# ("none" for the empty set), or logind's state of the account and the number of
# its sessions.
poll_names() {
    local previous=unset current
    while :; do
        current=$(names_of_account | sort | tr '\n' ' ')
        if [ "$current" != "$previous" ]; then
            printf '%s %s\n' "$(date +%s.%N)" "${current:-none}"
            previous=$current
        fi
        sleep 0.2
    done
}

poll_user() {
    local previous=unset current
    while :; do
        current="state=$(user_state) sessions=$(loginctl list-sessions --no-legend 2> /dev/null \
            | awk -v name="$account" '$3 == name { count++ } END { print count + 0 }')"
        if [ "$current" != "$previous" ]; then
            printf '%s %s\n' "$(date +%s.%N)" "$current"
            previous=$current
        fi
        sleep 0.2
    done
}

run_login() {
    local mode=$2 run_directory=$runs/$1
    env ${session_class:+"XDG_SESSION_CLASS=$session_class"} su -l "$account" -c "$out/login-body.sh $mode $run_directory $late_seconds $timeout_seconds" \
        > "$run_directory/login.stdout" 2>&1 9>&-
    echo "$?" > "$run_directory/login.status"
}

start_service_job() {
    local label=$1
    systemd-run --quiet --collect --unit="b10-job-$label" --uid="$account" --gid="$account" \
        "$out/job.sh" "$runs/$label" "$late_seconds" "$timeout_seconds" ${hold_seconds:+"$hold_seconds"} 9>&-
}

stop_job() {
    local label=$1 run_directory=$runs/$1
    systemctl stop "b10-job-$label.service" > /dev/null 2>&1
    if [ ! -s "$run_directory/job.status" ]; then
        pkill -KILL -u "$account" -x 'late-pe|osoe-launch|job.sh' > /dev/null 2>&1
    fi
}

relative() {
    awk -v time="$1" -v zero="$2" 'BEGIN { if (time == "") print "-"; else printf "%.1f", time - zero }'
}

last_outcome=none

summarize() {
    local label=$1 mode=$2 configuration=$3 zero=$4 zero_name=$5 run_directory=$runs/$1
    local first_names removed late_call job_exit returned ring_ok outcome
    first_names=$(awk '$2 != "none" { print $1; exit }' "$run_directory/names.txt")
    removed=$(awk '$2 != "none" { seen = 1; next } seen && $2 == "none" { print $1; exit }' \
        "$run_directory/names.txt")
    late_call=$(awk '/rank 1: calling shmem_init/ { print $1; exit }' "$run_directory/job.err" 2> /dev/null)
    job_exit=$(sed -n 's/^exit //p' "$run_directory/job.status" 2> /dev/null)
    returned=$(grep -c 'shmem_init returned' "$run_directory/job.err" 2> /dev/null)
    ring_ok=$(grep -c 'ring ok' "$run_directory/job.err" 2> /dev/null)
    if [ "${job_exit:-}" = 0 ] && [ "${ring_ok:-0}" = 4 ]; then
        outcome=completed
    elif grep -q 'timeout after' "$run_directory/job.err" 2> /dev/null; then
        outcome=stalled
    else
        outcome=other
    fi
    last_outcome=$outcome
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$label" "$mode" "$configuration" "$zero_name" \
        "$(relative "$first_names" "$zero")" "$(relative "$removed" "$zero")" \
        "$(relative "$late_call" "$zero")" "${job_exit:--}" "${returned:-0}" "$outcome" \
        >> "$runs/summary.tsv"
    log "$label: $outcome; from the $zero_name: names first seen +$(relative "$first_names" "$zero") s, gone +$(relative "$removed" "$zero") s, late PE calls shmem_init +$(relative "$late_call" "$zero") s; launcher exit ${job_exit:--}; ${returned:-0} PEs left shmem_init"
}

run_case() {
    local label=$1 mode=$2 configuration=$3 run_directory=$runs/$1
    install -d -m 0777 "$run_directory"
    if [ ! -e "/var/lib/systemd/linger/$account" ]; then
        wait_for_user_gone 60
    fi
    find /dev/shm -maxdepth 1 -user "$account" -name 'osoe.*' -delete
    local since
    since=$(date +%s)
    {
        echo "$configuration"
        describe_logind
        echo
    } > "$run_directory/configuration.txt"
    poll_names > "$run_directory/names.txt" 9>&- &
    local names_poller=$!
    poll_user > "$run_directory/user-state.txt" 9>&- &
    local user_poller=$!
    log "== $label ($mode): $configuration"
    run_login "$label" "$mode"
    local closed
    closed=$(date +%s.%N)
    echo "$closed" > "$run_directory/session.closed"
    if [ "$mode" = service ]; then
        start_service_job "$label"
    fi
    local deadline=$(($(date +%s) + timeout_seconds + 30))
    until [ -s "$run_directory/job.status" ]; do
        if [ "$(date +%s)" -ge "$deadline" ]; then
            log "$label: the job wrote no status in time"
            break
        fi
        sleep 0.5
    done
    sleep 1
    kill "$names_poller" "$user_poller" 2> /dev/null
    wait "$names_poller" "$user_poller" 2> /dev/null
    names_of_account > "$run_directory/leftover-names.txt"
    stop_job "$label"
    journalctl --no-pager -o short-precise --since "@$since" 2> /dev/null \
        | grep -E 'systemd-logind|user@|user-runtime-dir@|user-[0-9]+\.slice|session-[a-z0-9]+\.scope|b10-(job|login)-' \
        > "$run_directory/journal.txt"
    if [ "$mode" = held ]; then
        summarize "$label" "$mode" "$configuration" "$(cat "$run_directory/job.started")" "job's start"
    else
        summarize "$label" "$mode" "$configuration" "$closed" "session's end"
    fi
}

check_session_registration() {
    local run_directory=$runs/check
    install -d -m 0777 "$run_directory"
    run_login check service
    cat "$run_directory/login.txt"
    if ! grep -q '^XDG_SESSION_ID=.' "$run_directory/login.txt"; then
        log "no session id in the account's session: logind did not open one"
        return 1
    fi
    if ! awk '/^--- list-sessions/ { found = 1; next } /^--- / { found = 0 } found' \
        "$run_directory/login.txt" | grep -q " $account "; then
        log "logind's list of sessions does not show $account"
        return 1
    fi
    log "logind counts the account's session"
    wait_for_user_gone 60
}

# A job of the same four PEs with the late one one second late, as a service of
# the account and no session: the program and the tree work for the account
# before any run is counted.
smoke_job() {
    local run_directory=$runs/smoke
    install -d -m 0777 "$run_directory"
    systemd-run --quiet --collect --wait --unit=b10-job-smoke --uid="$account" --gid="$account" \
        "$out/job.sh" "$run_directory" 1 30 9>&-
    if [ "$(cat "$run_directory/job.status" 2> /dev/null)" = "exit 0" ] \
        && [ "$(grep -c 'ring ok' "$run_directory/job.err")" = 4 ]; then
        log "the smoke job completed"
    else
        log "the smoke job did not complete: $(cat "$run_directory/job.status" 2>&1); $(tail -n 5 "$run_directory/job.err" 2>&1)"
        return 1
    fi
}

# --- before and after ---------------------------------------------------------

preflight() {
    if [ "$(id -u)" != 0 ]; then log "not root"; return 1; fi
    if id "$account" > /dev/null 2>&1 || getent group "$account" > /dev/null; then
        log "$account exists already: not made by this run"
        return 1
    fi
    if [ ! -x "$tree/build/release/tools/launch/osoe-launch" ] || [ ! -x "$tree/build/release/late-pe" ]; then
        log "the tree $tree is not built"
        return 1
    fi
    if [ -e "$saved/logind.conf" ] && ! cmp -s "$saved/logind.conf" /etc/systemd/logind.conf; then
        log "$saved holds a logind.conf other than the current one: an earlier run did not finish; run undo"
        return 1
    fi
    if [ -e /etc/systemd/logind.conf.d ]; then log "/etc/systemd/logind.conf.d exists"; return 1; fi
    if ! grep -q '^#RemoveIPC=yes$' /etc/systemd/logind.conf; then
        log "logind.conf is not the shipped file"
        return 1
    fi
    if [ "$(logind_property RemoveIPC)" != true ]; then log "RemoveIPC is not at its default"; return 1; fi
    if [ -e "/var/lib/systemd/linger/$account" ]; then log "lingering is set for $account"; return 1; fi
    install -d -m 0755 "$out" "$runs"
    rm -rf "$saved"
    install -d -m 0700 "$saved"
    cp -a /etc/systemd/logind.conf "$saved/logind.conf"
    sha256sum /etc/systemd/logind.conf > "$saved/logind.conf.sha256"
    stat -c '%n %U:%G %a %s bytes, modified %y' /etc/systemd/logind.conf > "$saved/logind.conf.stat"
    (cd /etc && sha256sum passwd shadow group gshadow subuid subgid) > "$saved/etc-before.sha256"
    logind_property RemoveIPC > "$saved/remove-ipc.original"
    ls /var/log/journal/*/ 2> /dev/null | grep '^user-' > "$saved/user-journals.before"
    record_user_managers
    return 0
}

# Logind starts the manager of every lingering user when it starts, if it is not
# running: the restarts below started one that had been dead since it was killed
# on 2026-10-07. The ones not running now are stopped again by the undo.
record_user_managers() {
    local file name uid
    : > "$saved/user-managers.before"
    for file in /var/lib/systemd/linger/*; do
        if [ ! -e "$file" ]; then continue; fi
        name=$(basename "$file")
        uid=$(id -u "$name" 2> /dev/null) || continue
        printf '%s %s\n' "$uid" "$(systemctl is-active "user@$uid.service")" >> "$saved/user-managers.before"
    done
}

stop_started_user_managers() {
    local uid state
    if [ ! -e "$saved/user-managers.before" ]; then return 0; fi
    while read -r uid state; do
        if [ "$state" != active ] && [ "$(systemctl is-active "user@$uid.service")" = active ]; then
            systemctl stop "user@$uid.service"
            log "stopped user@$uid.service, which was not running before and which logind's restart started"
        fi
    done < "$saved/user-managers.before"
}

wait_for_quiet() {
    local waited=0 busy
    while :; do
        busy=$(ps -eo pid,user,etime,args | grep -E '[o]soe-launch|[c]test |[s]hmem_test_|[o]soe-bench|[c]c1 |[c]make --build| [m]ake ' \
            | grep -v "$tree/build/release/tools/launch" | head -5)
        if [ -z "$busy" ] || [ "$waited" -ge 120 ]; then break; fi
        sleep 5
        waited=$((waited + 5))
    done
    log "other work at the start (waited $waited s): ${busy:-none}"
}

record_environment() {
    {
        echo "# $(date -u +%Y-%m-%dT%H:%M:%SZ) in the osoe development VM"
        grep PRETTY_NAME /etc/os-release
        echo "kernel $(uname -r) $(uname -m)"
        systemctl --version | head -1
        dpkg -s systemd 2> /dev/null | grep '^Version'
        echo "--- logind's configuration, as systemd-analyze cat-config reads it"
        systemd-analyze cat-config systemd/logind.conf | grep -E '^# /|^[A-Za-z]+='
        echo "--- logind.conf as shipped"
        cat "$saved/logind.conf.stat"
        cat "$saved/logind.conf.sha256"
        echo "--- logind now"
        describe_logind
        echo
        echo "--- sessions and users now"
        loginctl list-sessions
        loginctl list-users
        echo "--- /etc/login.defs"
        grep -E '^(UID_MIN|UID_MAX|HOME_MODE|USERGROUPS_ENAB|SUB_UID_COUNT)' /etc/login.defs
        echo "--- the tree"
        cat "$out/build.status"
    } > "$runs/environment.txt" 2>&1
}

create_account() {
    if ! useradd --create-home --shell /bin/bash "$account"; then return 1; fi
    id -u "$account" > "$saved/account.uid"
    log "created: $(id "$account"); $(getent passwd "$account")"
    chmod 755 "$out/job.sh" "$out/login-body.sh"
}

undo() {
    log "undo: begin; $(describe_logind)"
    systemctl stop 'b10-job-*.service' 'b10-login-*.service' > /dev/null 2>&1
    if made_by_this_run; then
        if [ -e "/var/lib/systemd/linger/$account" ]; then
            loginctl disable-linger "$account"
            log "lingering disabled"
        fi
        loginctl terminate-user "$account" > /dev/null 2>&1
        sleep 1
        pkill -KILL -u "$account" > /dev/null 2>&1
        wait_for_user_gone 40
    fi
    if [ -e "$saved/logind.conf" ] && grep -q '^RemoveIPC=no$' /etc/systemd/logind.conf; then
        cp -a "$saved/logind.conf" /etc/systemd/logind.conf
        log "logind.conf restored"
    fi
    if [ -e "$saved/remove-ipc.original" ] \
        && [ "$(logind_property RemoveIPC)" != "$(cat "$saved/remove-ipc.original")" ]; then
        restart_logind
    fi
    if made_by_this_run; then
        userdel --remove "$account"
        log "account removed: userdel exit $?"
    fi
    stop_started_user_managers
    log "undo: end; $(describe_logind)"
}

verify() {
    local uid=""
    if [ -s "$saved/account.uid" ]; then uid=$(cat "$saved/account.uid"); fi
    echo "== the account"
    getent passwd "$account" || echo "no passwd entry for $account"
    getent group "$account" || echo "no group entry for $account"
    ls -ld "/home/$account" "/var/mail/$account" 2>&1
    echo "== lingering"
    ls -l /var/lib/systemd/linger
    echo "== logind"
    loginctl list-users
    loginctl list-sessions
    describe_logind
    echo
    echo "== logind.conf"
    ls -l --time-style=full-iso /etc/systemd/logind.conf
    cat "$saved/logind.conf.stat" 2> /dev/null
    sha256sum /etc/systemd/logind.conf
    cat "$saved/logind.conf.sha256" 2> /dev/null
    if [ ! -e "$saved/logind.conf.sha256" ]; then
        echo "logind.conf: no saved checksum"
    elif sha256sum -c --quiet "$saved/logind.conf.sha256" 2> /dev/null; then
        echo "logind.conf: the saved checksum matches"
    else
        echo "logind.conf: DIFFERS from the saved checksum"
    fi
    ls -d /etc/systemd/logind.conf.d 2>&1
    echo "== /etc's account files"
    if [ ! -e "$saved/etc-before.sha256" ]; then
        echo "the account files: no saved checksums"
    elif (cd /etc && sha256sum -c --quiet "$saved/etc-before.sha256"); then
        echo "passwd shadow group gshadow subuid subgid: the saved checksums match"
    else
        echo "an account file DIFFERS from its saved checksum"
    fi
    echo "== units, processes and files of the account"
    systemctl list-units --all --no-legend 'b10-*'
    if [ -n "$uid" ]; then
        ps -u "$uid" -o pid,args
        find /dev/shm /home /var/tmp /run/user -maxdepth 1 -uid "$uid" 2> /dev/null
        ls -d "/run/user/$uid" 2>&1
        ls /var/log/journal/*/ 2> /dev/null | grep "^user-$uid" || echo "no journal file of uid $uid"
    fi
}

finish() {
    local status=$?
    trap - EXIT
    undo
    verify > "$runs/verify-after-undo.txt" 2>&1
    echo "$status" > "$runs/FINISHED"
    log "finished with status $status"
    exit "$status"
}

# --- the modes ----------------------------------------------------------------

mode_run() {
    install -d -m 0755 "$runs"
    printf 'label\tmode\tconfiguration\ttimes_from\tnames_seen_s\tnames_gone_s\tlate_pe_calls_s\tlauncher_exit\tpes_through_init\toutcome\n' \
        > "$runs/summary.tsv"
    wait_for_quiet
    preflight || exit 1
    record_environment
    create_account || exit 1
    check_session_registration || exit 1
    smoke_job || exit 1

    local stock="logind as shipped: RemoveIPC=yes, no lingering"
    run_case a-service service "$stock"
    run_case a-login login "$stock"
    local login_outcome=$last_outcome
    run_case d-held held "$stock, the session held open for the whole job"

    loginctl enable-linger "$account"
    wait_for_state lingering 20
    log "lingering enabled: $(ls -l "/var/lib/systemd/linger/$account")"
    run_case b-service service "lingering enabled, RemoveIPC=yes"
    if [ "$login_outcome" = stalled ]; then
        run_case b-login login "lingering enabled, RemoveIPC=yes"
    fi
    loginctl disable-linger "$account"
    wait_for_user_gone 40

    set_remove_ipc_no || exit 1
    run_case c-service service "RemoveIPC=no in logind.conf, logind restarted, no lingering"
    if [ "$login_outcome" = stalled ]; then
        run_case c-login login "RemoveIPC=no in logind.conf, logind restarted, no lingering"
    fi
    restore_logind_conf || exit 1
    run_case e-service service "logind.conf restored and logind restarted: as shipped again"
}

# A job past its start when the account's last session ends: every PE calls
# shmem_init at once and holds 30 s after it. The program is compiled again
# first, for the hold.
mode_running() {
    install -d -m 0755 "$runs"
    printf 'label\tmode\tconfiguration\ttimes_from\tnames_seen_s\tnames_gone_s\tlate_pe_calls_s\tlauncher_exit\tpes_through_init\toutcome\n' \
        > "$runs/summary.tsv"
    "$tree/build/release/tools/wrapper/oshcc" -O2 -Wall -Wextra -o "$tree/build/release/late-pe" \
        "$out/late-pe.c" || exit 1
    wait_for_quiet
    preflight || exit 1
    record_environment
    create_account || exit 1
    check_session_registration || exit 1
    smoke_job || exit 1
    late_seconds=0
    hold_seconds=30
    run_case f-running service "logind as shipped; no PE late, every PE holds 30 s after shmem_init"
}

case ${1:-} in
check)
    trap finish EXIT
    trap 'exit 143' TERM INT HUP
    take_lock 1800 || exit 3
    wait_for_quiet
    preflight || exit 1
    record_environment
    create_account || exit 1
    check_session_registration || exit 1
    smoke_job || exit 1
    ;;
run)
    trap finish EXIT
    trap 'exit 143' TERM INT HUP
    take_lock 1800 || exit 3
    mode_run
    ;;
running)
    trap finish EXIT
    trap 'exit 143' TERM INT HUP
    take_lock 1800 || exit 3
    mode_running
    ;;
undo)
    take_lock 60
    undo
    ;;
verify)
    verify
    ;;
cleanup)
    take_lock 60
    uid=$(cat "$saved/account.uid" 2> /dev/null)
    if [ -n "$uid" ] && ! getent passwd "$uid" > /dev/null; then
        rm -f /var/log/journal/*/user-"$uid".journal /var/log/journal/*/user-"$uid"@*.journal
    fi
    rm -rf "$tree" "$out" "$saved"
    ;;
*)
    echo "usage: experiment.sh check | run | running | undo | verify | cleanup" >&2
    exit 2
    ;;
esac
