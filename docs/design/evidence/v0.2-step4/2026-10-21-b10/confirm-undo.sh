#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# B10: the osoe development VM after the experiment's last undo and `cleanup`,
# against what was read before anything was changed (2026-10-21, about 00:40 by
# the VM's clock, from the VM's own files):
#
#   sudo bash -s < confirm-undo.sh
#
# It prints one line a check, "ok" or "DIFFERS", and "all ok" or "SOMETHING
# DIFFERS" last. It changes nothing.
set -u
account=osoe-b10
failures=0

check() {   # description, then the command whose success is the "ok"
    local description=$1
    shift
    if "$@" > /dev/null 2>&1; then
        echo "ok       $description"
    else
        echo "DIFFERS  $description"
        failures=$((failures + 1))
    fi
}

equals() { [ "$1" = "$2" ]; }
absent() { [ ! -e "$1" ]; }

echo "# $(date -u +%Y-%m-%dT%H:%M:%SZ), $(systemctl --version | head -1)"

# The account
check "no passwd entry for $account" bash -c "! getent passwd $account"
check "no group entry for $account" bash -c "! getent group $account"
check "no home /home/$account" absent "/home/$account"
check "no mail spool /var/mail/$account" absent "/var/mail/$account"
check "no lingering file for $account" absent "/var/lib/systemd/linger/$account"
check "lingering files: vincent's alone (as before)" equals "$(ls /var/lib/systemd/linger)" vincent
check "no processes of uid 1000" bash -c "[ -z \"\$(ps -u 1000 -o pid=)\" ]"
check "no file of uid 1000 on the root file system, /tmp, /var/tmp, /dev/shm or /run" \
    bash -c "[ -z \"\$(find / /tmp /var/tmp /dev/shm /run -xdev -uid 1000 2> /dev/null | head -1)\" ]"
check "no journal file of uid 1000" bash -c "! ls /var/log/journal/*/user-1000* /run/log/journal/*/user-1000*"
check "no run directory /run/user/1000" absent /run/user/1000

# /etc's account files, as read before: sha256sum of each
check "/etc/passwd as before" equals "$(sha256sum < /etc/passwd | cut -d' ' -f1)" \
    55c1b1505f68237a00060aa3c4caaa1d29d26ad21498513926e3337d48847fdb
check "/etc/shadow as before" equals "$(sha256sum < /etc/shadow | cut -d' ' -f1)" \
    4f6677da3116657922f1a6947652b0d612bcc29951e2a9b6c3ff8235ac5866b9
check "/etc/group as before" equals "$(sha256sum < /etc/group | cut -d' ' -f1)" \
    7a51ff5577286f88aaed30b10ac31b7e9d11a674ddffbcb243faf29bde92fedc
check "/etc/gshadow as before" equals "$(sha256sum < /etc/gshadow | cut -d' ' -f1)" \
    e97c127accf7fda04ef2d02fd97de8cd4bd892a5b6c42600d3d138e3f3d8278f
check "/etc/subuid as before" equals "$(sha256sum < /etc/subuid | cut -d' ' -f1)" \
    8c7b203fa979282b0464fb5b44159739bce6ba5a6d02c8dc4c79d2f5145d1c1f
check "/etc/subgid as before" equals "$(sha256sum < /etc/subgid | cut -d' ' -f1)" \
    8c7b203fa979282b0464fb5b44159739bce6ba5a6d02c8dc4c79d2f5145d1c1f

# logind
check "/etc/systemd/logind.conf as before (checksum)" \
    equals "$(sha256sum < /etc/systemd/logind.conf | cut -d' ' -f1)" \
    a0f5aec31f2d016804a15e53bb603bd0fa68a4b5cb19deaa65a2cf6cb3466bca
check "/etc/systemd/logind.conf as before (owner, mode, size, modification time)" \
    equals "$(stat -c '%U:%G %a %s %y' /etc/systemd/logind.conf)" \
    "root:root 644 1785 2026-04-13 15:38:05.000000000 -0400"
check "no /etc/systemd/logind.conf.d" absent /etc/systemd/logind.conf.d
check "logind running with RemoveIPC=yes" equals "$(busctl get-property org.freedesktop.login1 \
    /org/freedesktop/login1 org.freedesktop.login1.Manager RemoveIPC)" "b true"
check "logind's UserStopDelayUSec 10 s" equals "$(busctl get-property org.freedesktop.login1 \
    /org/freedesktop/login1 org.freedesktop.login1.Manager UserStopDelayUSec)" "t 10000000"
check "logind's KillUserProcesses off" equals "$(busctl get-property org.freedesktop.login1 \
    /org/freedesktop/login1 org.freedesktop.login1.Manager KillUserProcesses)" "b false"
check "logind lists vincent's one session alone (as before)" \
    equals "$(loginctl list-sessions --no-legend | awk '{ print $3 }' | sort | uniq -c | tr -s ' ')" " 1 vincent"
check "logind lists vincent alone as a user" \
    equals "$(loginctl list-users --no-legend | awk '{ print $2 }')" vincent
check "vincent's user manager not running (as before: killed on 2026-10-07)" \
    bash -c "[ \"\$(systemctl is-active user@501.service)\" != active ]"

# The experiment's own leavings
check "no unit b10-*" bash -c "[ -z \"\$(systemctl list-units --all --no-legend 'b10-*')\" ]"
check "no /tmp/b10, /tmp/b10-out or /var/tmp/b10-saved" \
    bash -c "[ ! -e /tmp/b10 ] && [ ! -e /tmp/b10-out ] && [ ! -e /var/tmp/b10-saved ]"
check "the VM's lock file still there, untouched" bash -c "[ -f /tmp/osoe-vm.lock ] && [ ! -s /tmp/osoe-vm.lock ]"

if [ "$failures" = 0 ]; then
    echo "all ok"
else
    echo "SOMETHING DIFFERS: $failures"
fi
