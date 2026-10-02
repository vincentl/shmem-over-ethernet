#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# The notice that the connection buffers were shrunk to fit OSOE_AM_BUFFER_BUDGET
# is printed once a node, by its lead, and not once a PE (connection-area.c;
# the release review's P3: 128,000 lines at 500 nodes of 256 PEs).
#
#   shrink-notice.sh LAUNCHER PROGRAM
#
# Three logical nodes of three PEs each, so that a line from every PE would be
# nine and one from every node is three. A buffer of 1 MiB is asked for and the
# budget is 1 MiB, where a node's two buffers to each of its two peers come to
# 4 MiB: the buffers are halved to 256 KiB, and the job says so. PROGRAM is
# any program that initializes, once, and finishes; the job must succeed, since
# a shrunk buffer is a working one.
set -u
LAUNCHER=$1 PROGRAM=$2
nodes=3 per_node=3
out=$(OSOE_AM_BUFFER=1048576 OSOE_AM_BUFFER_BUDGET=1048576 \
    "$LAUNCHER" -n $((nodes * per_node)) -m 0,0,0,1,1,1,2,2,2 -t 60 -- "$PROGRAM" 2>&1)
status=$?
printf '%s\n' "$out"
echo "launcher exited $status"
notices=$(printf '%s\n' "$out" | grep -c 'connection buffers reduced from 1048576 to 262144 bytes')
echo "shrink notices: $notices, for $nodes nodes of $per_node PEs"
[ "$status" -eq 0 ] && [ "$notices" -eq "$nodes" ] || exit 1
echo "SHRINK NOTICE OK"
