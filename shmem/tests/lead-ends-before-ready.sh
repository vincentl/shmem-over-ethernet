#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# lead-ends-before-ready.sh SHIM NAME PROGRAM [ARGS]...
#
# PROGRAM, except that rank 0, its node's lead, runs with SHIM preloaded
# (kill-on-create.c) and NAME in OSOE_TEST_KILL_NAME, which ends it by SIGKILL
# when it creates an object whose name contains NAME - with ".heap." its own
# heap, which it creates after it has made and sized the node's control segment
# and before it stores `ready`. The other ranks have mapped the segment and
# wait on a `ready` that cannot come, and must end by themselves naming the
# lead (node.c, lead_ended_before_ready; the release fixes' review, correctness
# F2).
#
# Rank 0 is run, not exec'd, and the script ends with status 4 when PROGRAM is
# gone: for refusal.sh the lead is then a rank that ended with a failure
# status, and not one the launcher killed. A PROGRAM the shim never ends leaves
# the others to finish the start, and the test fails.
SHIM=$1 KILL_NAME=$2
shift 2
if [ "$OSOE_PROCID" = 0 ]; then
    LD_PRELOAD=$SHIM OSOE_TEST_KILL_NAME=$KILL_NAME "$@"
    exit 4
fi
exec "$@"
