#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# The zero-copy send among many peers at the locked-memory limit a site has
# (T39; the scale review of v0.2 before Part 4, S8; test-zero-copy-refusals.c):
#
#   zero-copy-refusals.sh LAUNCHER PROGRAM NODES LIMIT_KIB PUT_KIB ROUNDS LEAST
#
# NODES logical nodes of one PE each, every process's limit on locked memory
# LIMIT_KIB, soft and hard - the hard one too, since shmem_init raises a soft
# limit to it - with OSOE_WIRE_ZERO_COPY=on, and the deadlock detector off as
# the other zero-copy tests run it, so that a wait the fallback stranded would
# be the launcher's timeout and not a sweep's rescue. The job must end well. The
# notice that a send was refused for want of locked memory, which each process
# says once and which names its limit, must come from LEAST of the nodes at
# least, each naming LIMIT_KIB, and "Cannot allocate memory", which a refusal
# ended the job with before the wire copied after one, must not appear.
set -u
LAUNCHER=$1 PROGRAM=$2 NODES=$3 LIMIT=$4 PUT=$5 ROUNDS=$6 LEAST=$7

# A limit the shell cannot set is a limit this test cannot hold the job to: a
# user whose hard limit is below LIMIT_KIB has less than the test means to give.
hard=$(ulimit -H -l)
if [ "$hard" != unlimited ] && [ "$hard" -lt "$LIMIT" ]; then
    echo "SKIPPED: this shell's hard limit on locked memory, $hard KiB, is below the $LIMIT KiB this test runs under"
    exit 0
fi
ulimit -l "$LIMIT" || { echo "cannot set the limit on locked memory to $LIMIT KiB"; exit 1; }

map=$(seq -s, 0 $((NODES - 1)))
out=$(OSOE_WIRE_ZERO_COPY=on OSOE_DEADLOCK_SECONDS=0 SHMEM_SYMMETRIC_SIZE=16m \
    "$LAUNCHER" -n "$NODES" -m "$map" -t 240 -- "$PROGRAM" "$LIMIT" "$PUT" "$ROUNDS" 2>&1)
status=$?

notice='a zero-copy send was refused for want of locked memory'
said=$(printf '%s\n' "$out" | grep -c "$notice")
named=$(printf '%s\n' "$out" | grep -c "$notice.*(ulimit -l, $LIMIT KiB here)")

# What a failure shows: all of it, the notices too.
fail() {
    printf '%s\n' "$out"
    echo "launcher exited $status; nodes saying a send was refused: $said of $NODES, $named naming the limit of $LIMIT KiB"
    echo "$1"
    exit 1
}

# The program skips, alike on every PE, where a process may lock memory whatever
# its limit.
case $out in *"SKIPPED: "*) printf '%s\n' "$out"; exit 0 ;; esac

# Linux 6.14 and later charge a ring to the same count (ring.c and
# transport-internode.c say so): where the rings of this many nodes alone pass
# the limit, the job is refused at start-up, naming it, and no send is ever made
# to be refused. That is a limit of this machine, not a finding.
if [ "$status" -ne 0 ] &&
   printf '%s\n' "$out" | grep -Eq 'Linux 6[.]14 and later (can be|io_uring counts)[^"]*locked-memory limit'; then
    printf '%s\n' "$out"
    echo "SKIPPED: a ring could not be made under the $LIMIT KiB limit, which this kernel charges rings to: $NODES nodes cannot start, so no send is refused"
    exit 0
fi

[ "$status" -eq 0 ] || fail "the job did not end well"
printf '%s\n' "$out" | grep -q 'Cannot allocate memory' &&
    fail "a connection failed for want of memory: no copy after the refusal"
printf '%s\n' "$out" | grep -q 'ZERO COPY PUTS OK' ||
    fail "PE 0 did not say that every PE's data was right"
[ "$said" -ge "$LEAST" ] ||
    fail "fewer than $LEAST nodes met a refusal: the limit was not reached, and the fallback not tried"
[ "$named" -eq "$said" ] ||
    fail "a notice named another limit than $LIMIT KiB"

# The notices are one a process, so one is shown and the rest counted.
printf '%s\n' "$out" | grep -v "$notice"
printf '%s\n' "$out" | grep -m 1 "$notice"
echo "launcher exited $status; nodes saying a send was refused: $said of $NODES, $named naming the limit of $LIMIT KiB"
echo "ZERO COPY REFUSALS OK"
