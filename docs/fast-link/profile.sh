#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §13: one node's CPUs profiled for SECONDS while a job runs
# beside it (behind.sh, common.sh's beside) - every process, with call graphs -
# then the profile's top lines by thread name, library and symbol, each prefixed
# by the node's name, so that the progress threads (osoe-agent, osoe-helper-N)
# are told from the PEs' own. PERF is the command (default perf; "sudo -n perf"
# where kernel.perf_event_paranoid keeps a user from every process); where it
# is missing or refused, the line says so.
# usage: profile.sh SECONDS
set -u
PERF=${PERF:-perf}
data=/tmp/osoe-kit-perf.$$.data
say() { sed "s/^/$(hostname) /"; }
# shellcheck disable=SC2086
if ! $PERF record -a -g -F 499 -o "$data" -- sleep "$1" > /dev/null 2> "$data.err"; then
    { echo "profile: $PERF record failed:"; head -5 "$data.err"; } | say
    rm -f "$data" "$data.err"
    exit 0
fi
# shellcheck disable=SC2086
$PERF report -i "$data" --stdio --no-children --sort comm,dso,symbol --percent-limit 0.5 2>&1 |
    grep -v '^#' | grep . | head -80 | say
rm -f "$data" "$data.err"
