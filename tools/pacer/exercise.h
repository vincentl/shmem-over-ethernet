/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The interface every osoe-pacer exercise implements, and what the driver,
 * osoe-pacer.c, gives the exercises in return.
 *
 * An exercise is a name and five functions, and the driver owns the loop:
 *
 *   memory      before anything runs, on every PE alike: fit the exercise's
 *               sizes to the budget, lowering what it may lower (the sort's
 *               keys a round, the alltoall's largest block), and return the
 *               symmetric bytes it will then use - UINT64_MAX when even its
 *               smallest does not fit, with the reason in `fields`, which
 *               otherwise hold its line in the header
 *   setup       allocate, on the exercise's team
 *   step        do `units` units of work - a round of the sort, one exchange
 *               of every variant at one block size for the alltoall - recording
 *               failures through pacer_failure; `last` says no step follows, so
 *               that anything left open (the accumulating sort's cycle) closes
 *   checkpoint  this PE's counters since the last call; called on every
 *               member together, so an exercise may reduce, or print lines of
 *               its own from the team's rank 0, before the driver's line
 *   teardown    free
 *
 * After each step the driver reduces the counters over the team - the sums of
 * work, bytes, batches and failures; the largest of each phase's seconds, of
 * the batches and of the step itself (the shortest step would say nothing:
 * every step ends in a collective of the team) - prints the checkpoint line
 * from the team's rank 0 and the failure lines through parallel_print, and
 * adapts `units` so that a step takes about --report seconds. Under --rounds a
 * checkpoint is a round. Nothing here needs a thread.
 */
#ifndef PACER_EXERCISE_H
#define PACER_EXERCISE_H

#include <stddef.h>
#include <stdint.h>

#include <shmem.h>

enum pacer_sort_mode { PACER_SORT_ROUNDS, PACER_SORT_ACCUMULATE };
enum pacer_exchange { PACER_EXCHANGE_FETCH_ADD, PACER_EXCHANGE_ALLTOALL };

/* The planted defects of --plant (the brief's decision 9, and three more the
 * author added on 2026-09-27 so that every check has one): each makes its
 * check fire - the capacity one the oracle too, since refused keys are lost -
 * so that a ctest run can assert it still does. */
enum pacer_plant {
    PACER_PLANT_NONE,
    PACER_PLANT_SORT_KEY,           /* PE 1 changes a key after counting it */
    PACER_PLANT_SORT_ORDER,         /* PE 1 skips its first local sort */
    PACER_PLANT_SORT_RANGE,         /* PE 1 sends a key to the PE after its owner */
    PACER_PLANT_SORT_CAPACITY,      /* PE 1 reserves, or declares, more than a PE holds */
    PACER_PLANT_ALLTOALL_BYTE,      /* PE 1 flips a byte of one block, each variant */
    PACER_PLANT_ALLTOALL_GAP,       /* PE 1 writes a word into a gap of a strided dest */
    PACER_PLANT_PRINT_HASH,         /* PE 1 hashes other text than it prints */
    PACER_PLANT_MAILBOX_PAYLOAD,    /* PE 1 flips a byte of its first message */
    PACER_PLANT_MAILBOX_SIGNAL,     /* PE 1 signals one message twice over */
    PACER_PLANT_MAILBOX_COUNT,      /* PE 1 counts one message it read twice */
    PACER_PLANTS
};

struct pacer_options {
    double seconds;                 /* each exercise about this long ... */
    long rounds;                    /* ... or this many rounds, when not 0 */
    double report;                  /* seconds between checkpoints */
    uint64_t memory;                /* the budget: bytes a PE, symmetric and private each */
    uint64_t seed;
    int quick;                      /* the smoke test's sizes */
    enum pacer_sort_mode sort_mode;
    enum pacer_exchange exchange;
    int use_ptr;                    /* 0 under --no-ptr */
    int stop_on_failure;
    enum pacer_plant plant;
};

/* parallel_print's text capacity, NUL included, as the driver sets it. */
enum { PACER_MESSAGE_CAPACITY = 1024 };

/* The phases an exercise may time, named by its `phases` list. */
enum { PACER_PHASES = 5 };

struct pacer_counters {
    double work;                    /* in the exercise's own unit: keys, exchanges, cases */
    double bytes;                   /* moved between PEs, for the rate */
    double batch;                   /* what one PE received, for the imbalance; 0 if none */
    double phase_seconds[PACER_PHASES];
};

struct pacer_exercise {
    const char *name;
    const char *work;               /* what `work` counts, named in the line */
    const char *const *phases;      /* phase names, NULL-terminated, at most PACER_PHASES */
    long quick_rounds;              /* rounds under --quick */
    uint64_t (*memory)(int npes, const struct pacer_options *options, char *fields,
                       size_t size);
    int (*setup)(shmem_team_t team, const struct pacer_options *options);
    void (*step)(long units, int last);
    void (*checkpoint)(struct pacer_counters *counters);
    void (*teardown)(void);
};

extern const struct pacer_exercise pacer_sort, pacer_alltoall, pacer_mailbox, pacer_print;

/*
 * The job's shape, from SHMEM_TEAM_SHARED at start: every world PE's node,
 * nodes numbered in the order of their leads' world ranks (a node's lead is
 * its shared team's rank 0), and its rank on its node.
 */
struct pacer_layout {
    int pes;
    int nodes;
    const int *node;                /* [pes] */
    const int *local;               /* [pes] */
};
const struct pacer_layout *pacer_layout(void);

/* Seconds on a monotonic clock. */
double pacer_now(void);

/*
 * A failure found by this PE: counted, and the first few lines kept for the
 * next checkpoint, which prints them through parallel_print on the exercise's
 * team and clears them. The text is `exercise round=r what: value`, with what
 * differs between PEs last, so that the same failure on many PEs is one line.
 */
void pacer_failure(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* A line for the end of the run, after the driver's summary of the exercise:
 * kept on the calling PE, which prints it. */
void pacer_summary(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* Bytes spelled as SHMEM_SYMMETRIC_SIZE spells them: exactly when they are a
 * whole number of a unit ("256m", "1500m", "4k"), else to a tenth of the
 * largest unit, rounded up ("72.1m"), and plain below a kibibyte. */
void pacer_size(char *text, size_t size, uint64_t bytes);

#endif /* PACER_EXERCISE_H */
