/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * parallel_print (parallel-print.h; osoe-pacer proposal §2.2), one collective
 * a call:
 *
 *   1. Format into a private buffer; L is its length, 0 when silent.
 *   2. h = FNV-1a 64 over the bytes, then MurmurHash3's fmix64, so that every
 *      bit depends on every byte - FNV alone leaves short texts that differ in
 *      their last bytes close together. 0 means silent, so a hash of 0 is 1.
 *   3. shmem_fcollectmem of {h, L}: every member holds every member's record,
 *      in team rank order, in an array sized for the world at init.
 *   4. Locally, alike on every member since the records are: the owner of a
 *      hash is the lowest rank with it; D owners; their texts packed in rank
 *      order into windows of the arena, none straddling two, so that each
 *      member knows its own offset and the number of windows W - usually one.
 *   5. For each window: its owners put their text, NUL included, into rank 0's
 *      arena and quiet; everyone syncs; rank 0 prints the window's lines; and
 *      if another window follows, everyone syncs again before it is written.
 *
 * **No sync after the last window.** Rank 0 prints before it enters its next
 * call, and an owner's next put into rank 0's arena follows that owner's
 * return from the next call's fcollect, which needs rank 0's record - so rank
 * 0 has entered it, and printed.
 *
 * **But every call syncs at least once**, a silent one too. The spec wants a
 * collect's dest ready on every member before any member calls (§9.10.8), and
 * the next call's fcollect writes the array this one reads: so each member
 * copies what it needs out of the records before its first sync, and a sync
 * always follows. The same rule is why a program that calls on one team and
 * then on another must sync the second team in between, when the second has a
 * member the first did not: that member waits for nobody, and its fcollect may
 * write a record array another member is still reading. osoe writes a peer's
 * buffer only once the peer has entered, so this never shows on osoe; the
 * spec does not promise it.
 *
 * **Collisions.** Two different texts with one hash print once, under the
 * union of both groups' labels, with the lower rank's text. With D distinct
 * texts in a call the odds are about D^2 / 2^65 - one in 10^11 at ten thousand
 * distinct lines. Accepted for a console. The fix, if it is ever needed, is a
 * 128-bit record: two independent finalizers over the text, 24 bytes a PE.
 *
 * **The signal route** (proposal §2.5) replaces step 3's fcollect with the
 * tool's own dissemination: each member holds the records of the ranks from
 * its own on, and in round k puts the first min(2^k, n - 2^k) of them to the
 * member 2^k ranks before it, into the same places 2^k along, with
 * shmem_putmem_signal adding one to a signal word of the round's, and waits
 * for the member 2^k after to do the same. After ceil(log2 n) rounds each
 * holds all n, which it copies into team rank order: the fcollect's array,
 * by other means. A round has its own word, since the rounds' puts come from
 * different members and may land in any order; each word counts every put
 * this PE has taken in its round, against a count of its own calls that had
 * that round. Nothing of the next call can land before this call's last wait:
 * the sync every call ends with needs this PE, as it does for the fcollect.
 *
 * **Folding to nodes** is rank 0's alone, a group at a time: a tally of the
 * group's PEs by host against each host's PEs, which init counted, then the
 * hosts' names folded - O(k log k) for a group of k. A group is folded only
 * when whole and when the names fit the room its PE numbers would have had,
 * so a line never outgrows its buffer.
 *
 * Memory, a PE, N the world: 32 N bytes symmetric - the fcollect's array and
 * the signal route's - with its 32 signal words, the 64 KiB arena, and
 * about 130 N private (the sorted records, each owner's first record, a
 * group's PEs, the host names and each PE's host, a host's PEs, a group's
 * tally and its names), with the text and line buffers. Every member sorts
 * the records, O(n log n) for a team of n; the labels' translation to world
 * ranks is rank 0's alone.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <shmem.h>

#include "parallel-print.h"

enum { ARENA = 64 * 1024, HOST = 64, NAMES = 64, NAME = 32, SMALLEST = 16, ROUNDS = 32 };

struct record {
    uint64_t hash;
    uint64_t length;
};

/* A member's record, sorted by hash and then by rank, so that each group is a
 * run whose first entry is its owner's. */
struct entry {
    uint64_t hash;
    uint32_t rank;
    uint32_t length;
};

static struct {
    int ready;
    int world;
    size_t capacity;                /* the text buffer, NUL included */
    size_t window;                  /* bytes of the arena one window may fill */
    struct record *records;         /* symmetric, [world] */
    struct record *mine;            /* symmetric, one */
    char *arena;                    /* symmetric, ARENA bytes */
    char *text;                     /* [capacity] */
    struct entry *entries;          /* [world] */
    int *first;                     /* [world]: an owner's first entry; -1 for the rest */
    int *members;                   /* [world]: one group's world PEs */
    char *line;                     /* a label and a text */
    size_t line_size;
    size_t label_room;              /* of line_size, what a label may take */
    char *hosts;                    /* [world][HOST] */
    int *host_of;                   /* [world]: a PE's host, among the distinct names */
    int *host_pes;                  /* [hosts]: the PEs a host has */
    int *tally;                     /* [hosts]: a group's PEs a host; 0 between groups */
    const char **fold_names;        /* [world]: a group's hosts, one PE's name each */
    int fold;
    struct {
        shmem_team_t team;
        char name[NAME];
    } names[NAMES];
    int named;
    char *capture;
    size_t capture_size;
    size_t captured;
    int plant_hash;
    int plant_fold;
    int plant_route;
    enum parallel_print_route route;
    struct record *held;            /* symmetric, [world]: the signal route's records */
    uint64_t *signals;              /* symmetric, [ROUNDS]: a word a round */
    uint64_t expected[ROUNDS];      /* the puts each word has had by this call's end */
} state = { .window = ARENA };

/* A host name taken apart for folding: pi-003 is "pi-", 3, width 3. */
struct host {
    const char *name;
    size_t prefix;                  /* the name's length before its trailing digits */
    unsigned long long number;
    int width;                      /* digits kept with leading zeros; 0 when natural */
    int numbered;
};

static uint64_t hash_text(const char *text, size_t length, int planted)
{
    uint64_t h = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < length; i++) {
        h ^= (unsigned char)text[i];
        h *= UINT64_C(1099511628211);
    }
    if (planted) {                  /* the text and one byte more */
        h ^= '!';
        h *= UINT64_C(1099511628211);
    }
    h ^= h >> 33;
    h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33;
    h *= UINT64_C(0xc4ceb9fe1a85ec53);
    h ^= h >> 33;
    return h ? h : 1;
}

static int by_hash_then_rank(const void *a, const void *b)
{
    const struct entry *x = a, *y = b;
    if (x->hash != y->hash)
        return x->hash < y->hash ? -1 : 1;
    return (x->rank > y->rank) - (x->rank < y->rank);
}

static int ascending(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

/* World PEs by host name, then by number. */
static int by_host_then_pe(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    int c = strcmp(state.hosts + (size_t)x * HOST, state.hosts + (size_t)y * HOST);
    return c ? c : (x > y) - (x < y);
}

/* Prefixes in order, a name without a number before one with, then widths,
 * then numbers: so that one bracket's names are adjacent, in order. */
static int host_order(const void *a, const void *b)
{
    const struct host *x = a, *y = b;
    size_t shorter = x->prefix < y->prefix ? x->prefix : y->prefix;
    int c = strncmp(x->name, y->name, shorter);
    if (c)
        return c;
    if (x->prefix != y->prefix)
        return x->prefix < y->prefix ? -1 : 1;
    if (x->numbered != y->numbered)
        return x->numbered - y->numbered;
    if (x->width != y->width)
        return x->width - y->width;
    return (x->number > y->number) - (x->number < y->number);
}

/* snprintf at out + *used, counting on past size what did not fit. */
static void append(char *out, size_t size, size_t *used, const char *format, ...)
    __attribute__((format(printf, 4, 5)));
static void append(char *out, size_t size, size_t *used, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int n = *used < size ? vsnprintf(out + *used, size - *used, format, arguments)
                         : vsnprintf(NULL, 0, format, arguments);
    va_end(arguments);
    if (n > 0)
        *used += (size_t)n;
}

size_t parallel_print_fold_names(const char *const *names, int count, char *out, size_t size)
{
    if (size)
        out[0] = '\0';
    struct host *hosts = malloc(sizeof *hosts * (size_t)(count > 0 ? count : 1));
    if (!hosts) {
        size_t used = 0;
        append(out, size, &used, "?");
        return used;
    }
    for (int i = 0; i < count; i++) {
        struct host h = { .name = names[i] };
        size_t length = strlen(h.name), start = length;
        while (start > 0 && h.name[start - 1] >= '0' && h.name[start - 1] <= '9')
            start--;
        h.prefix = start;
        h.numbered = start < length && length - start < 19;
        if (h.numbered) {
            h.number = strtoull(h.name + start, NULL, 10);
            h.width = h.name[start] == '0' && length - start > 1 ? (int)(length - start) : 0;
        } else {
            h.prefix = length;
        }
        hosts[i] = h;
    }
    qsort(hosts, (size_t)count, sizeof *hosts, host_order);
    int distinct = 0;               /* one name is one order: duplicates are adjacent */
    for (int i = 0; i < count; i++)
        if (distinct == 0 || strcmp(hosts[distinct - 1].name, hosts[i].name) != 0)
            hosts[distinct++] = hosts[i];

    size_t used = 0;
    for (int i = 0; i < distinct;) {
        int j = i;                  /* one prefix and width: one bracket */
        while (j + 1 < distinct && hosts[i].numbered && hosts[j + 1].numbered &&
               hosts[j + 1].prefix == hosts[i].prefix && hosts[j + 1].width == hosts[i].width &&
               strncmp(hosts[j + 1].name, hosts[i].name, hosts[i].prefix) == 0)
            j++;
        if (i > 0)
            append(out, size, &used, ",");
        if (j == i) {
            append(out, size, &used, "%s", hosts[i].name);
        } else {
            append(out, size, &used, "%.*s[", (int)hosts[i].prefix, hosts[i].name);
            for (int k = i; k <= j;) {
                int m = k;
                while (m + 1 <= j && hosts[m + 1].number == hosts[m].number + 1)
                    m++;
                int width = state.plant_fold ? 0 : hosts[k].width;
                append(out, size, &used, "%s%0*llu", k > i ? "," : "", width, hosts[k].number);
                if (m > k)
                    append(out, size, &used, "-%0*llu", width, hosts[m].number);
                k = m + 1;
            }
            append(out, size, &used, "]");
        }
        i = j + 1;
    }
    free(hosts);
    return used;
}

int parallel_print_init(size_t capacity)
{
    if (state.ready)
        return -1;
    int world = shmem_n_pes();
    state.world = world;
    state.capacity = capacity < SMALLEST ? SMALLEST : capacity;
    /* Symmetric allocations are collective and alike on every PE, so they
     * fail alike too. */
    state.records = shmem_malloc(sizeof *state.records * (size_t)world);
    state.mine = shmem_malloc(sizeof *state.mine);
    state.held = shmem_malloc(sizeof *state.held * (size_t)world);
    state.signals = shmem_calloc(ROUNDS, sizeof *state.signals);
    state.arena = shmem_malloc(ARENA);
    char *host = shmem_malloc(HOST);
    char *hosts = shmem_malloc((size_t)HOST * (size_t)world);
    /* A label is at most a prefix, a bracket, and every PE of the world with a
     * separator; 12 characters hold any int and its comma. A label by hosts
     * gets the same room, or is not used. */
    state.label_room = NAME + 48 + (size_t)world * 12;
    state.line_size = state.label_room + state.capacity + 4;
    state.text = malloc(state.capacity);
    state.entries = malloc(sizeof *state.entries * (size_t)world);
    state.first = malloc(sizeof *state.first * (size_t)world);
    state.members = malloc(sizeof *state.members * (size_t)world);
    state.line = malloc(state.line_size);
    state.hosts = malloc((size_t)HOST * (size_t)world);
    state.host_of = malloc(sizeof *state.host_of * (size_t)world);
    state.host_pes = calloc((size_t)world, sizeof *state.host_pes);
    state.tally = calloc((size_t)world, sizeof *state.tally);
    state.fold_names = malloc(sizeof *state.fold_names * (size_t)world);
    if (!state.records || !state.mine || !state.held || !state.signals || !state.arena ||
        !host || !hosts || !state.text ||
        !state.entries || !state.first || !state.members || !state.line || !state.hosts ||
        !state.host_of || !state.host_pes || !state.tally || !state.fold_names)
        return -1;

    memset(host, 0, HOST);
    if (gethostname(host, HOST - 1) != 0)
        snprintf(host, HOST, "unknown");
    host[strcspn(host, ".")] = '\0';
    if (shmem_fcollectmem(SHMEM_TEAM_WORLD, hosts, host, HOST) != 0)
        return -1;
    memcpy(state.hosts, hosts, (size_t)HOST * (size_t)world);
    shmem_free(hosts);
    shmem_free(host);

    /* Each PE's host, numbered among the distinct names, and each host's PEs:
     * the PEs sorted by name, a host a run. members serves as the order. */
    for (int pe = 0; pe < world; pe++)
        state.members[pe] = pe;
    qsort(state.members, (size_t)world, sizeof *state.members, by_host_then_pe);
    int distinct = 0;
    for (int i = 0; i < world; i++) {
        int pe = state.members[i];
        if (i == 0 || strcmp(state.hosts + (size_t)state.members[i - 1] * HOST,
                             state.hosts + (size_t)pe * HOST) != 0)
            distinct++;
        state.host_of[pe] = distinct - 1;
        state.host_pes[distinct - 1]++;
    }
    state.ready = 1;
    return 0;
}

int parallel_print_name_team(shmem_team_t team, const char *name)
{
    for (int i = 0; i < state.named; i++) {
        if (state.names[i].team != team)
            continue;
        if (name)
            snprintf(state.names[i].name, NAME, "%s", name);
        else
            state.names[i] = state.names[--state.named];
        return 0;
    }
    if (!name)
        return 0;
    if (state.named == NAMES)
        return -1;
    state.names[state.named].team = team;
    snprintf(state.names[state.named].name, NAME, "%s", name);
    state.named++;
    return 0;
}

/* The prefix of a team's labels, which every member would compute alike. */
static void team_prefix(shmem_team_t team, int n, char *out, size_t size)
{
    out[0] = '\0';
    if (team == SHMEM_TEAM_WORLD)
        return;
    if (team == SHMEM_TEAM_SHARED) {
        snprintf(out, size, "shared");
        return;
    }
    for (int i = 0; i < state.named; i++)
        if (state.names[i].team == team) {
            snprintf(out, size, "%s", state.names[i].name);
            return;
        }
    /* Every team a split of the world makes is a progression in world ranks;
     * only a split of SHMEM_TEAM_SHARED may not be, and it says so. */
    int start = shmem_team_translate_pe(team, 0, SHMEM_TEAM_WORLD);
    int stride = n > 1 ? shmem_team_translate_pe(team, 1, SHMEM_TEAM_WORLD) - start : 1;
    int progression = 1;
    for (int r = 2; r < n && progression; r++)
        progression = shmem_team_translate_pe(team, r, SHMEM_TEAM_WORLD) == start + r * stride;
    if (progression)
        snprintf(out, size, "team(%d:%d:%d)", start, stride, n);
    else
        snprintf(out, size, "team(%d:?:%d)", start, n);
}

static void emit(const char *line, size_t length)
{
    if (!state.capture) {
        fwrite(line, 1, length, stdout);
        fflush(stdout);
        return;
    }
    size_t room = state.capture_size - 1 - state.captured;
    if (length > room)
        length = room;
    memcpy(state.capture + state.captured, line, length);
    state.captured += length;
    state.capture[state.captured] = '\0';
}

/* The signal route: every member's record into state.records, in team rank
 * order, by dissemination (above). */
static void disseminate(shmem_team_t team, int n, int me)
{
    int rounds = 0;
    while (rounds < ROUNDS && (1 << rounds) < n)
        rounds++;
    for (int k = 0; k < rounds; k++)
        state.expected[k]++;
    state.held[0] = *state.mine;
    if (state.plant_route)          /* the planted defect: this route's record changed */
        state.held[0].hash ^= 1;
    for (int k = 0; k < rounds; k++) {
        int span = 1 << k, count = span < n - span ? span : n - span;
        if (k > 0)                  /* what this round sends arrived in the rounds before */
            shmem_signal_wait_until(&state.signals[k - 1], SHMEM_CMP_GE, state.expected[k - 1]);
        int to = shmem_team_translate_pe(team, (me - span + n) % n, SHMEM_TEAM_WORLD);
        shmem_putmem_signal(state.held + span, state.held, sizeof *state.held * (size_t)count,
                            &state.signals[k], 1, SHMEM_SIGNAL_ADD, to);
    }
    if (rounds > 0)
        shmem_signal_wait_until(&state.signals[rounds - 1], SHMEM_CMP_GE,
                                state.expected[rounds - 1]);
    for (int j = 0; j < n; j++)
        state.records[(me + j) % n] = state.held[j];
}

/* Rank 0, folding to nodes: the label of the group in members[0, k) by its
 * hosts' names, when it holds every PE of each host it touches and they fit
 * the label's room. 1 when written at out + *used; 0, with nothing written,
 * when the group keeps its PE numbers. */
static int fold_label(const char *prefix, int k, char *out, size_t size, size_t *used)
{
    for (int i = 0; i < k; i++)
        state.tally[state.host_of[state.members[i]]]++;
    int whole = 1, names = 0;
    for (int i = 0; i < k; i++) {
        int h = state.host_of[state.members[i]];
        if (state.tally[h] < 0)
            continue;               /* its host is named already */
        whole &= state.tally[h] == state.host_pes[h];
        state.fold_names[names++] = state.hosts + (size_t)state.members[i] * HOST;
        state.tally[h] = -1;
    }
    for (int i = 0; i < k; i++)
        state.tally[state.host_of[state.members[i]]] = 0;
    if (!whole)
        return 0;
    size_t start = *used;
    if (prefix[0])
        append(out, size, used, "%s@", prefix);
    size_t room = state.label_room > *used ? state.label_room - *used : 0;
    size_t needed = parallel_print_fold_names(state.fold_names, names, out + *used, room);
    if (needed >= room) {
        *used = start;
        out[start] = '\0';
        return 0;
    }
    *used += needed;
    return 1;
}

/* Rank 0: the lines of one window, walking the owners as the packing did. */
static void print_window(shmem_team_t team, int n, int count, int window)
{
    char prefix[NAME + 48];
    team_prefix(team, n, prefix, sizeof prefix);
    size_t offset = 0;
    int w = -1;
    for (int r = 0; r < n; r++) {
        if (state.first[r] < 0)
            continue;
        const struct entry *owner = &state.entries[state.first[r]];
        size_t bytes = (size_t)owner->length + 1;
        if (w < 0 || offset + bytes > state.window) {
            w++;
            offset = 0;
        }
        if (w == window) {
            int k = 0;
            for (int i = state.first[r]; i < count && state.entries[i].hash == owner->hash; i++)
                state.members[k++] =
                    shmem_team_translate_pe(team, (int)state.entries[i].rank, SHMEM_TEAM_WORLD);
            int sorted = 1;
            for (int i = 1; i < k && sorted; i++)
                sorted = state.members[i - 1] < state.members[i];
            if (!sorted)
                qsort(state.members, (size_t)k, sizeof *state.members, ascending);

            char *out = state.line;
            size_t size = state.line_size, used = 0;
            const char *after = " ";
            if (!state.fold || !fold_label(prefix, k, out, size, &used)) {
                used += (size_t)snprintf(out + used, size - used, "%s[", prefix);
                for (int i = 0; i < k && used < size;) {
                    int j = i;
                    while (j + 1 < k && state.members[j + 1] == state.members[j] + 1)
                        j++;
                    used += (size_t)snprintf(out + used, size - used, i ? ",%d" : "%d",
                                             state.members[i]);
                    if (j > i && used < size)
                        used += (size_t)snprintf(out + used, size - used, "-%d",
                                                 state.members[j]);
                    i = j + 1;
                }
                after = "] ";
            }
            /* Bounded by the length the owner declared, not by its NUL: text
             * that did not arrive must not read past its slot. */
            if (used < size)
                used += (size_t)snprintf(out + used, size - used, "%s%.*s\n", after,
                                         (int)owner->length, state.arena + offset);
            if (used >= size)
                used = size - 1;
            emit(out, used);
        }
        offset += bytes;
    }
}

int parallel_print(shmem_team_t team, const char *format, ...)
{
    if (!state.ready || team == SHMEM_TEAM_INVALID)
        return -1;
    int n = shmem_team_n_pes(team), me = shmem_team_my_pe(team);
    if (n < 1 || me < 0)
        return -1;

    /* 1. Format, cut to fit a window and the buffer both. */
    size_t limit = state.capacity < state.window ? state.capacity : state.window;
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(state.text, limit, format, arguments);
    va_end(arguments);
    size_t length = written < 0 ? 0 : (size_t)written;
    if (length >= limit) {
        length = limit - 1;
        memcpy(state.text + length - 3, "...", 3);
    }
    while (length > 0 && state.text[length - 1] == '\n')    /* each line has its own */
        length--;
    state.text[length] = '\0';

    /* 2 and 3. */
    state.mine->hash = length ? hash_text(state.text, length, state.plant_hash) : 0;
    state.mine->length = length;
    if (state.route == PARALLEL_PRINT_SIGNAL)
        disseminate(team, n, me);
    else if (shmem_fcollectmem(team, state.records, state.mine, sizeof *state.mine) != 0)
        return -1;

    /* 4. Everything needed from the records is copied out here, before the
     * first sync. A length no window holds could only come from a member with
     * another window; it is cut alike on every member. */
    int count = 0;
    for (int r = 0; r < n; r++) {
        state.first[r] = -1;
        if (state.records[r].hash == 0)
            continue;
        uint64_t bytes = state.records[r].length;
        state.entries[count].hash = state.records[r].hash;
        state.entries[count].rank = (uint32_t)r;
        state.entries[count].length = (uint32_t)(bytes < state.window ? bytes : state.window - 1);
        count++;
    }
    qsort(state.entries, (size_t)count, sizeof *state.entries, by_hash_then_rank);
    int distinct = 0;
    for (int i = 0; i < count; i++)
        if (i == 0 || state.entries[i].hash != state.entries[i - 1].hash) {
            state.first[state.entries[i].rank] = i;
            distinct++;
        }
    size_t offset = 0, my_offset = 0;
    int windows = 0, my_window = -1;
    for (int r = 0; r < n; r++) {
        if (state.first[r] < 0)
            continue;
        size_t bytes = (size_t)state.entries[state.first[r]].length + 1;
        if (windows == 0 || offset + bytes > state.window) {
            windows++;
            offset = 0;
        }
        if (r == me) {
            my_window = windows - 1;
            my_offset = offset;
        }
        offset += bytes;
    }

    /* 5. */
    if (me == 0 && state.capture) {
        state.captured = 0;
        state.capture[0] = '\0';
    }
    int root = shmem_team_translate_pe(team, 0, SHMEM_TEAM_WORLD);
    for (int w = 0; w < windows; w++) {
        if (my_window == w) {
            size_t bytes = state.entries[state.first[me]].length;
            state.text[bytes] = '\0';
            shmem_putmem(state.arena + my_offset, state.text, bytes + 1, root);
            shmem_quiet();
        }
        shmem_team_sync(team);
        if (me == 0)
            print_window(team, n, count, w);
        if (w + 1 < windows)
            shmem_team_sync(team);
    }
    if (windows == 0)
        shmem_team_sync(team);
    return distinct;
}

void parallel_print_finalize(void)
{
    if (!state.ready)
        return;
    shmem_free(state.arena);
    shmem_free(state.signals);
    shmem_free(state.held);
    shmem_free(state.mine);
    shmem_free(state.records);
    free(state.fold_names);
    free(state.tally);
    free(state.host_pes);
    free(state.host_of);
    free(state.hosts);
    free(state.line);
    free(state.members);
    free(state.first);
    free(state.entries);
    free(state.text);
    state.ready = 0;
}

const char *parallel_print_host(int pe)
{
    if (!state.ready || pe < 0 || pe >= state.world)
        return "?";
    return state.hosts + (size_t)pe * HOST;
}

size_t parallel_print_window(size_t bytes)
{
    size_t previous = state.window;
    state.window = bytes < SMALLEST ? SMALLEST : bytes > ARENA ? ARENA : bytes;
    return previous;
}

void parallel_print_capture(char *buffer, size_t size)
{
    state.capture = size ? buffer : NULL;
    state.capture_size = size;
    state.captured = 0;
    if (state.capture)
        state.capture[0] = '\0';
}

void parallel_print_plant_hash(int on)
{
    state.plant_hash = on;
}

int parallel_print_fold_nodes(int on)
{
    int previous = state.fold;
    state.fold = on;
    return previous;
}

void parallel_print_plant_fold(int on)
{
    state.plant_fold = on;
}

enum parallel_print_route parallel_print_route(enum parallel_print_route route)
{
    enum parallel_print_route previous = state.route;
    state.route = route;
    return previous;
}

void parallel_print_plant_route(int on)
{
    state.plant_route = on;
}
