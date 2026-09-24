#!/bin/sh
# dead-pe-init.sh LAUNCHER PROGRAM
#
# Runs test-dead-pe.c's init case on 0,0,1: rank 0, node 0's lead, dies of
# SIGALRM inside shmem_init, after its node's rendezvous and while the job's
# is held open by node 1 starting late. Rank 1 waits on its lead there, before
# any agent runs, and must find the death itself - its sweep's pidfd
# (liveness.h) - and leave with the failure status, not be killed by the
# launcher. Node 1's rank, which cannot reach a dead lead, is left to the
# launcher's timeout. The lead dies 3 s in and
# node 1 starts 8 s late: a lead that died inside its node's rendezvous, which
# no pidfd watches yet (liveness.h), would leave rank 1 to the launcher too.
LAUNCHER=$1 PROGRAM=$2
out=$("$LAUNCHER" -n 3 -m 0,0,1 -t 12 -k -- "$PROGRAM" 0 init 2>&1)
echo "$out"
died=$(printf '%s\n' "$out" | grep -c "rank 0 killed by signal 14\$")
found=$(printf '%s\n' "$out" | grep -c "PE 1: local PE 0 (rank 0, pid [0-9]*) has ended without finalizing")
left=$(printf '%s\n' "$out" | grep -c "rank 1 exited with status 1\$")
echo "dead lead in init: died $died, found $found, rank 1 left $left"
[ "$died" -eq 1 ] && [ "$found" -eq 1 ] && [ "$left" -eq 1 ] || exit 1
echo "DEAD LEAD IN INIT OK"
