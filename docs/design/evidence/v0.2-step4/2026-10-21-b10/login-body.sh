#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# B10: what the throwaway account's login session runs - the command of
# `su -l ACCOUNT -c`, whose PAM stack (pam_systemd) opens the session and closes
# it when this script returns.
#
#   login-body.sh MODE RUN_DIRECTORY LATE_SECONDS TIMEOUT_SECONDS
#
# MODE
#   service  only look at the session and hold it for two seconds; the job is
#            started outside it, by the controller, once it has ended (as
#            Slurm's daemon starts a job: not a child of any login)
#   login    start the job in the background from this session (setsid, nohup),
#            hold the session two seconds, and leave: the job goes on in the
#            session's scope, with no session
#   held     run the job in the foreground, so that the session lasts as long
#            as the job does
#
# login.txt in RUN_DIRECTORY says what logind made of this session.
mode=$1
run_directory=$2
late_seconds=$3
timeout_seconds=$4
here=$(dirname "$0")
{
    echo "id: $(id)"
    echo "XDG_SESSION_ID=${XDG_SESSION_ID:-}"
    echo "--- cgroup of this shell"
    cat /proc/self/cgroup
    echo "--- list-sessions"
    loginctl list-sessions --no-legend
    echo "--- session-status"
    if [ -n "${XDG_SESSION_ID:-}" ]; then
        loginctl session-status "$XDG_SESSION_ID" --no-pager
    fi
} > "$run_directory/login.txt" 2>&1
case $mode in
service)
    sleep 2
    ;;
login)
    setsid nohup "$here/job.sh" "$run_directory" "$late_seconds" "$timeout_seconds" \
        > /dev/null 2>&1 < /dev/null &
    sleep 2
    ;;
held)
    "$here/job.sh" "$run_directory" "$late_seconds" "$timeout_seconds"
    ;;
esac
