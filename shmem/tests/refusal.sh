#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# A start-up that goes wrong must end, or say why it cannot (the round-three
# review's R8 and N1; design §3.15, §6.2):
#
#   refusal.sh LAUNCHER RANKS WAITING [PATTERN]... -- LAUNCHER-ARGS... -- PROGRAM [ARGS]...
#
# runs `osoe-launch -k LAUNCHER-ARGS -- PROGRAM [ARGS]...` and requires
#
#   WAITING 0   every one of the RANKS ranks to end by itself, with a failure
#               status: the launcher's timeout never comes and none is killed.
#               The refusal of a PE reaches its node-mates, and a refusing lead
#               leaves its callers an end of stream, or the job would hang.
#   WAITING w   the launcher's timeout to come with exactly w ranks still
#               running, and the other ranks to have ended by themselves. These
#               are the ones that wait for a node or a PE that will not come,
#               which the notice of their wait must name before the timeout
#               (base/stall-watch.h; the tests make it come after
#               OSOE_TEST_NOTICE_SECONDS) - nothing gives up (§3.15).
#   WAITING l-h as many as l to h, for a start-up whose outcome for a node that
#               called a lead as it ended depends on which of two callers
#               arrived first: a hello that was answered ends its caller, and
#               one that was not is retried against a closed port.
#
# and each PATTERN, an extended regular expression, to appear in what the job
# wrote before the launcher's timeout, so that what a rank wrote on its way
# down under the launcher's SIGKILL does not count. A PATTERN that begins with
# ! must appear nowhere.
#
# -k is what this is for. Without it the launcher ends the survivors of the
# first failure, and a start-up that hangs looks like one that ended: the suite
# could not see R8. LAUNCHER-ARGS must not give -k again.
#
# WRAP, if set, is put before the launcher: tier 3 runs it under
# tools/launch/tier3.sh (NAME=VALUE arguments of the wrapper are the job's
# environment there, as sudo does not pass one on).
LAUNCHER=$1 RANKS=$2 WAITING=$3
shift 3
patterns=$(mktemp) || exit 1
trap 'rm -f "$patterns"' EXIT
while [ $# -gt 0 ] && [ "$1" != "--" ]; do
    printf '%s\n' "$1" >> "$patterns"
    shift
done
[ $# -gt 0 ] || { echo "refusal.sh: no -- after the patterns" >&2; exit 2; }
shift

# shellcheck disable=SC2086 # WRAP is a command with its arguments
out=$(${WRAP:-} "$LAUNCHER" -k "$@" 2>&1)
echo "$out"

count() { printf '%s\n' "$out" | grep -c "$1"; }
timeouts=$(count '^osoe-launch: timeout after ')
killed=$(count '^osoe-launch: rank [0-9]* killed by signal ')
ended=$(count '^osoe-launch: rank [0-9]* exited with status ')
# What the job said before the launcher ended it.
before=$(printf '%s\n' "$out" | sed '/^osoe-launch: timeout after /,$d')

unmet=0
fail() { echo "refusal.sh: $*"; unmet=$((unmet + 1)); }
case $WAITING in
*-*) low=${WAITING%-*} high=${WAITING#*-} ;;
*) low=$WAITING high=$WAITING ;;
esac
# A rank that waits is one the launcher kills at its timeout; any other signal
# is a crash, which no wait ends in and which is never right.
if [ "$killed" -lt "$low" ] || [ "$killed" -gt "$high" ]; then
    fail "$killed rank(s) were left to the launcher's timeout or killed, wanted $WAITING"
fi
if [ "$killed" -eq 0 ]; then
    [ "$timeouts" -eq 0 ] || fail "the launcher's timeout came, with no rank left running"
else
    [ "$timeouts" -eq 1 ] || fail "$timeouts timeouts of the launcher, wanted one"
    count "^osoe-launch: timeout after [0-9]* s; killing $killed process" > /dev/null ||
        fail "the launcher's timeout did not find $killed rank(s) still running"
fi
[ "$ended" -eq $((RANKS - killed)) ] ||
    fail "$ended ranks ended by themselves, wanted $((RANKS - killed))"
while IFS= read -r pattern; do
    case $pattern in
    '!'*)
        if printf '%s\n' "$out" | grep -Eq -e "${pattern#!}"; then
            fail "written, though it must not be: ${pattern#!}"
        fi ;;
    *)
        printf '%s\n' "$before" | grep -Eq -e "$pattern" ||
            fail "not written before the launcher's timeout: $pattern" ;;
    esac
done < "$patterns"

echo "refusal: of $RANKS ranks, $ended ended by themselves and $killed were killed by the" \
     "launcher ($timeouts timeout); $unmet requirement(s) unmet"
[ "$unmet" -eq 0 ] || exit 1
echo "REFUSAL OK"
