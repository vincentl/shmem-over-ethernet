/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The OSOE_ names osoe knows, and the notice for any other (unknown-settings.h).
 *
 * The list is every OSOE_ name the tree reads or sets in a PE's environment,
 * and nothing else: shmem.settings-known (cmake/CheckSettingsKnown.cmake)
 * collects them from the library, the tools and the test suite as
 * shmem.settings-documented collects the library's and the tools', and fails
 * on a name that is read and not listed here - which would draw this notice
 * from every job that sets it - and on a name listed here that nothing reads.
 * Both checks leave this file out of what they collect.
 */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "fatal.h"
#include "unknown-settings.h"

extern char **environ;

static const char *const known_settings[] = {
    /* The library's settings: the README's "Environment variables". */
    "OSOE_AGENT_CPUS",
    "OSOE_AGENT_SLICE_US",
    "OSOE_AM_BUFFER",
    "OSOE_AM_BUFFER_BUDGET",
    "OSOE_AM_CHUNK",
    "OSOE_AM_CREDITS",
    "OSOE_AM_HUGEPAGES",
    "OSOE_CHECK_QUIET",
    "OSOE_COLLECTIVE_FANOUT",
    "OSOE_COLLECTIVE_PIPELINE",
    "OSOE_COLLECTIVE_PLACES",
    "OSOE_COLLECTIVE_SHAPE",
    "OSOE_DEADLOCK_SECONDS",
    "OSOE_FORCE_REMOTE",
    "OSOE_GET_LANDING_BYTES",
    "OSOE_HEAP_BASE",
    "OSOE_HEAP_SHIFT",
    "OSOE_JOB_SECRET",
    "OSOE_KEEPALIVE",
    "OSOE_KEEP_CONTROL_SEGMENT",
    "OSOE_LINK_BYTES_PER_SECOND",
    "OSOE_LINK_HOP_NS",
    "OSOE_LINK_MESSAGE_NS",
    "OSOE_MAX_CONTEXTS",
    "OSOE_MAX_TEAMS",
    "OSOE_MAX_THREADS",
    "OSOE_MESH_SLOT",
    "OSOE_NO_PROGRESS_TIMEOUT",
    "OSOE_PORT_RANGE",
    "OSOE_PROGRESS_HELPERS",
    "OSOE_SHM_CAPACITY_CHECK",
    "OSOE_SMALL_COLLECTIVE_BYTES",
    "OSOE_TEAM_BLOCKS",
    "OSOE_TEST_ABORT_DELAY_MS",
    "OSOE_TEST_ARCHITECTURE",
    "OSOE_TEST_DEADLOCK_DROPS",
    "OSOE_TEST_DEAF_HELPER",
    "OSOE_TEST_HOLD_GOODBYE",
    "OSOE_TEST_MESH_HOLD",
    "OSOE_TEST_NOTICE_SECONDS",
    "OSOE_TEST_PERTURB",
    "OSOE_TEST_PRINT_LINK",
    "OSOE_TEST_TEAM_GENERATION",
    "OSOE_WIRE_BUFFERS",
    "OSOE_WIRE_BYTES",
    "OSOE_WIRE_SEED",
    "OSOE_WIRE_SPLIT",
    "OSOE_WIRE_UNSENT_BYTES",
    "OSOE_WIRE_ZERO_COPY",
    "OSOE_WIRE_ZERO_COPY_BYTES",
    /* The launcher's: the README's "Starting PEs without a launcher". */
    "OSOE_GTIDS",
    "OSOE_JOB_ID",
    "OSOE_LAUNCH",
    "OSOE_LOCALID",
    "OSOE_NODEID",
    "OSOE_PROCID",
    "OSOE_STEP_ID",
    "OSOE_STEP_NODELIST",
    "OSOE_STEP_NUM_NODES",
    "OSOE_STEP_NUM_TASKS",
    /* The tools': oshcc's, osoe-launch's, the corpus's and osoe-pacer's. */
    "OSOE_CC",
    "OSOE_CORPUS",
    "OSOE_NODE_MAP",
    "OSOE_TEST_PACER_LATE_START",
    /* Retired, and said so by their own notice (bootstrap.c). */
    "OSOE_BOOTSTRAP_TIMEOUT",
    "OSOE_LIVENESS",
    "OSOE_WATCHDOG",
    /* The test suite's own, which its programs read: no setting of the
     * library's, and set in the PEs' environment by the tests alone. */
    "OSOE_TEST_AGREEMENT_WORD",
    "OSOE_TEST_ALLTOALL_STRIDE",
    "OSOE_TEST_AMO_NBI_MISUSE",
    "OSOE_TEST_BAD_ROOT",
    "OSOE_TEST_BAD_TEAM",
    "OSOE_TEST_BLOCKED_SECONDS",
    "OSOE_TEST_BULK_BYTES",
    "OSOE_TEST_CONTEXT",
    "OSOE_TEST_DEADLINE_MS",
    "OSOE_TEST_DIVERGE",
    "OSOE_TEST_DIVERGE_KIND",
    "OSOE_EXTRA",               /* osoe-launch's own test of its -e */
    "OSOE_TEST_FLOOR_PINS",
    "OSOE_TEST_FORGE",
    "OSOE_TEST_GETS",
    "OSOE_TEST_GET_BYTES",
    "OSOE_TEST_GET_NBI",
    "OSOE_TEST_GET_PIECES",
    "OSOE_TEST_HEAP_BYTES",
    "OSOE_TEST_HELD_RANK",
    "OSOE_TEST_HELPERS",
    "OSOE_TEST_IBPUT_MISUSE",
    "OSOE_TEST_LEADS",
    "OSOE_TEST_LOCKED_MEMORY",
    "OSOE_TEST_LOCK_MISUSE",
    "OSOE_TEST_MAP",
    "OSOE_TEST_MISUSE",
    "OSOE_TEST_MODE",
    "OSOE_TEST_MULTIPLE",
    "OSOE_TEST_PARTIAL",
    "OSOE_TEST_PLACES_LINK_WAY",
    "OSOE_TEST_POOL",
    "OSOE_TEST_PUT_BYTES",
    "OSOE_TEST_READY",
    "OSOE_TEST_SIGNAL",
    "OSOE_TEST_SLOW_AFTER",
    "OSOE_TEST_SLOW_NAME",
    "OSOE_TEST_SLOW_TEXT",
    "OSOE_TEST_SPLIT",
    "OSOE_TEST_STAT",
    "OSOE_TEST_STATICS_MISUSE",
    "OSOE_TEST_STOP_MS",
    "OSOE_TEST_STRIDE_MISUSE",
    "OSOE_TEST_THREE_OF_FIVE",
    "OSOE_TEST_VAR",
    "OSOE_TEST_WAIT",
};

#define KNOWN_COUNT (sizeof known_settings / sizeof known_settings[0])

/* The longest name the notice measures against the list; a longer one is
 * still said, with no nearer name offered. */
#define NAME_LONGEST 63

bool osoe_unknown_settings_known(const char *name)
{
    for (size_t i = 0; i < KNOWN_COUNT; i++)
        if (strcmp(known_settings[i], name) == 0)
            return true;
    return false;
}

/* The edit distance between two names - letters inserted, removed or changed -
 * by the two-row table; both at most NAME_LONGEST letters. */
static size_t distance(const char *a, size_t a_len, const char *b, size_t b_len)
{
    size_t previous[NAME_LONGEST + 1], current[NAME_LONGEST + 1];
    for (size_t j = 0; j <= b_len; j++)
        previous[j] = j;
    for (size_t i = 1; i <= a_len; i++) {
        current[0] = i;
        for (size_t j = 1; j <= b_len; j++) {
            size_t change = previous[j - 1] + (a[i - 1] != b[j - 1]);
            size_t removed = previous[j] + 1, inserted = current[j - 1] + 1;
            size_t least = change < removed ? change : removed;
            current[j] = least < inserted ? least : inserted;
        }
        memcpy(previous, current, (b_len + 1) * sizeof current[0]);
    }
    return previous[b_len];
}

/* The known name nearest `name`, if one is at most two edits away - a letter
 * dropped, doubled, changed or two swapped - or NULL. */
static const char *nearest(const char *name, size_t len)
{
    if (len > NAME_LONGEST)
        return NULL;
    const char *best = NULL;
    size_t best_distance = 3;
    for (size_t i = 0; i < KNOWN_COUNT; i++) {
        size_t known_len = strlen(known_settings[i]);
        if (known_len > NAME_LONGEST)
            continue;
        size_t d = distance(name, len, known_settings[i], known_len);
        if (d < best_distance) {
            best_distance = d;
            best = known_settings[i];
        }
    }
    return best;
}

void osoe_unknown_settings_notice(void)
{
    /* Once a process: a program that starts the library again after
     * shmem_finalize has the same environment. One thread runs init. */
    static bool said;
    if (said)
        return;
    said = true;
    for (char **entry = environ; entry && *entry; entry++) {
        if (strncmp(*entry, "OSOE_", 5) != 0)
            continue;
        const char *equals = strchr(*entry, '=');
        size_t len = equals ? (size_t)(equals - *entry) : strlen(*entry);
        char name[NAME_LONGEST + 1];
        if (len <= NAME_LONGEST) {
            memcpy(name, *entry, len);
            name[len] = '\0';
            if (osoe_unknown_settings_known(name))
                continue;
        }
        const char *near = nearest(*entry, len);
        if (near)
            osoe_notice("%.*s is not a variable osoe reads, and changes nothing: did you mean "
                        "%s?", (int)len, *entry, near);
        else
            osoe_notice("%.*s is not a variable osoe reads, and changes nothing (SHMEM_INFO=1 "
                        "lists the ones it reads)", (int)len, *entry);
    }
}
