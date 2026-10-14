#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# A line a job must write exactly so many times - a notice said once a process
# or once a job, and not once a call:
#
#   count-lines.sh COUNT PATTERN COMMAND [ARGS]...
#
# runs COMMAND, prints what it wrote, and requires it to have succeeded and to
# have written exactly COUNT lines that match PATTERN, an extended regular
# expression.
set -u
COUNT=$1 PATTERN=$2
shift 2
out=$("$@" 2>&1)
status=$?
printf '%s\n' "$out"
found=$(printf '%s\n' "$out" | grep -cE -- "$PATTERN")
echo "count-lines.sh: the command exited $status, $found line(s) matching, wanted $COUNT"
[ "$status" -eq 0 ] && [ "$found" -eq "$COUNT" ] && echo "COUNT OK"
