#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# H23's mutants (2026-10-27): each a breakage of data_wire_model.py's kind,
# made in the C of /tmp/h23 (the dev tree, built in debug), the library rebuilt,
# and the tests of what it breaks run; then the file restored from a copy of
# the pristine one - no git in the VM - and rebuilt. A mutant whose sed changed
# nothing is reported as such, never as caught or survived.
#   mutants.sh [NAME...]
set -u
exec 9>/tmp/osoe-vm.lock
flock -w 10800 9 || exit 1
TREE=/tmp/h23
OUT=/tmp/h23-mutants.summary
cd "$TREE" || exit 2
: > "$OUT"
say() { echo "$*" | tee -a "$OUT"; }
# name~file~sed expression~test regex[~second file~its expression]
MUTANTS=(
"open_early~shmem/src/progress.c~s/if (w->fd < 0 \&\& !attach_or_ask(o, i, \&attached))/if (0 \&\& w->fd < 0 \&\& !attach_or_ask(o, i, \&attached))/~^shmem\.data-wires\.ring\.0-1-2-3-4$"
"both_dial~shmem/src/data-wires.c~s/    if (osoe_mesh_schedule_calls(wires.node, peer, wires.nodes)) {/    if (1) {/~^shmem\.data-wires\.skip\.0-1-2-3-4$"
"no_ask~shmem/src/data-wires.c~s/        osoe_progress_ask_data_wire(\&osoe_am.progress, peer);/        (void)peer;/~^shmem\.data-wires\.skip\.0-1-2-3-4$"
"call_per_need~shmem/src/data-wires.c~s/    if (wires.state\[slot\] != SLOT_NONE || wires.aborting)/    if (wires.aborting)/~^shmem\.data-wires\.(ring|skip)\.0-1-2-3-4$"
"no_visit~shmem/src/data-wires.c~/    osoe_connection_area_mark(\&osoe_am.area, peer);/{N;s/.*/    (void)peer;/}~^shmem\.data-wires\.(ring|skip)\.0-1-2-3-4$"
"no_receive~shmem/src/progress.c~s/        osoe_ring_arm_receive(o->ring, i);/        (void)0;/~^shmem\.data-wires\.ring\.0-1-2-3-4$"
"abort_waits_wire~shmem/src/data-wires.c~s/    if (osoe_listener_answered(wires.listener) > 0)/    if (osoe_listener_answered(wires.listener) > 0 || wires.asked || wires.queue_count)/~^shmem\.data-wires\.exit$"
"quiet_while_making~shmem/src/progress.c~s/            if (c->wire_wanted)/            if (0 \&\& c->wire_wanted)/~^shmem\.data-wires\.stall-long$"
"quiet_while_making_twice~shmem/src/progress.c~s/            if (c->wire_wanted)/            if (0 \&\& c->wire_wanted)/~^shmem\.data-wires\.stall-long$~shmem/src/progress-host.c~s/!osoe_data_wires_making()/true/g"
"no_agreement~shmem/src/progress-host.c~s/        if (((const unsigned char \*)osoe_job_backend_blob(\&osoe.job, n))\[sizeof(uint16_t)\] != first)/        if (0)/~^shmem\.data-wires\.disagree\.0-1$"
"abandon_answered~shmem/src/listener.c~s/        if (l->callers\[i\].fd >= 0 \&\& !l->callers\[i\].answered)/        if (l->callers[i].fd >= 0)/~^shmem\.data-wires\.exit$"
"no_goodbye_new_wire~shmem/src/data-wires.c~s/    (void)send(fd, wires.goodbye, wires.goodbye_bytes, MSG_NOSIGNAL | MSG_DONTWAIT);/    (void)wires.goodbye_bytes;/~^shmem\.data-wires\.exit$"
"helper_done_early~shmem/src/progress-host.c~s/        bool closed = osoe_data_wires_handover_closed();/        bool closed = true;/~^shmem\.data-wires\.ring\.0-0-1-1-2-2-3-3\.helpers-2$"
"no_unsent_at_attach~shmem/src/progress-host.c~s/    if (asked)\$/    if (0 \&\& asked)/~^shmem\.(data-wires|unsent)\."
"helper_want_unseen~shmem/src/progress-host.c~s/    return owner == 0 \&\& osoe_data_wires_wanted();/    return false;/~^shmem\.data-wires\.ring\.0-0-1-1-2-2-3-3\.helpers-2$"
)
want=" $* "
for m in "${MUTANTS[@]}"; do
    IFS='~' read -r name file expr regex file2 expr2 <<< "$m"
    [ $# -gt 0 ] && [[ "$want" != *" $name "* ]] && continue
    cp "$file" "/tmp/h23-pristine.$(basename "$file")"
    sed -i -e "$expr" "$file"
    if cmp -s "$file" "/tmp/h23-pristine.$(basename "$file")"; then
        say "$name: THE SED CHANGED NOTHING - not run"
        continue
    fi
    if [ -n "${file2:-}" ]; then
        cp "$file2" "/tmp/h23-pristine.$(basename "$file2")"
        sed -i -e "$expr2" "$file2"
        if cmp -s "$file2" "/tmp/h23-pristine.$(basename "$file2")"; then
            say "$name: THE SECOND SED CHANGED NOTHING - not run"
            cp "/tmp/h23-pristine.$(basename "$file")" "$file"
            continue
        fi
    fi
    if ! cmake --build build/debug -j4 > "/tmp/h23-mutant-$name-build.txt" 2>&1; then
        say "$name: does not build ($(grep -m1 'error' "/tmp/h23-mutant-$name-build.txt"))"
    else
        OSOE_MESH_DATA_WIRES=on-demand ctest --test-dir build/debug -R "$regex" -j4 --timeout 120 \
            > "/tmp/h23-mutant-$name.txt" 2>&1
        failed=$(grep -cE '\(Failed\)|\(Timeout\)' "/tmp/h23-mutant-$name.txt")
        ran=$(grep -E 'tests passed' "/tmp/h23-mutant-$name.txt")
        if [ "$failed" -gt 0 ]; then
            say "$name: CAUGHT - $ran; $(grep -E '\(Failed\)|\(Timeout\)' "/tmp/h23-mutant-$name.txt" | sed 's/^ *[0-9]* - //' | tr '\n' ' ')"
        else
            say "$name: survived - $ran"
        fi
    fi
    cp "/tmp/h23-pristine.$(basename "$file")" "$file"
    [ -n "${file2:-}" ] && cp "/tmp/h23-pristine.$(basename "$file2")" "$file2"
done
cmake --build build/debug -j4 > /tmp/h23-mutant-restore-build.txt 2>&1 && say "restored and rebuilt" || say "RESTORE BUILD FAILED"
echo done > /tmp/h23-mutants.status
