#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §13: one node's CPUs profiled for SECONDS while a job runs
# beside it (behind.sh, common.sh's beside) - every process, with call graphs -
# then the profile's top lines by thread name, library and symbol, a flat table,
# each prefixed by the node's name, so that the progress threads (osoe-agent,
# osoe-helper-N) are told from the PEs' own; and after it the progress threads'
# own symbols with their callers, which say what a wake or a copy is made for
# (§20), perf's warnings on unwinding left out. PERF is the command (default perf; "sudo -n perf" where
# kernel.perf_event_paranoid keeps a user from every process); where it is
# missing or refused, the line says so. The data goes in a directory of the
# user's own, which the user can empty whoever wrote the file in it.
# usage: profile.sh SECONDS
set -u
PERF=${PERF:-perf}
dir=$(mktemp -d /tmp/osoe-kit-perf.XXXXXX) || exit 0
data=$dir/perf.data
say() { sed "s/^/$(hostname) /"; }
# shellcheck disable=SC2086
if ! $PERF record -a -g -F 499 -o "$data" -- sleep "$1" > /dev/null 2> "$dir/err"; then
    { echo "profile: $PERF record failed:"; head -5 "$dir/err"; } | say
    rm -rf "$dir"
    exit 0
fi
# shellcheck disable=SC2086
$PERF report -i "$data" --stdio --no-children -g none --sort comm,dso,symbol --percent-limit 0.5 2>&1 |
    grep -v '^#' | grep . | head -80 | say
echo "the progress threads' symbols, with their callers:" | say
# shellcheck disable=SC2086
$PERF report -i "$data" --stdio --no-children -g graph,2,callee --sort symbol \
    --comms osoe-agent,osoe-helper-1,osoe-helper-2,osoe-helper-3,osoe-helper-4 --percent-limit 2 2> /dev/null |
    grep -v '^#' | grep . | head -60 | say
rm -rf "$dir"
