#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# exit-before-init.sh RANK PROGRAM [ARGS]...
#
# PROGRAM, except that rank RANK of the job exits with status 3 before it: a PE
# that never reaches shmem_init, for refusal.sh. No library code runs in it, so
# nothing of it can tell its node what became of it (the round-three review's
# R8): the notices of the PEs that wait for it are all there is.
[ "$OSOE_PROCID" = "$1" ] && exit 3
shift
exec "$@"
