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
    PACER_PLANT_MAILBOX_ROUND_TRIP, /* PE 1 sends one round-trip message wrong twice */
    PACER_PLANT_THREADS_ROW,        /* PE 1's thread 0 puts one word other than it checks */
    PACER_PLANT_THREADS_TABLE,      /* PE 1 writes one word of its table wrong */
    PACER_PLANT_THREADS_COUNT,      /* PE 1's thread 0 counts one add short */
    PACER_PLANT_TEAMS_QUERY,        /* PE 1 expects every answer about one team off by one */
    PACER_PLANT_TEAMS_COLLECTIVE,   /* PE 1 gives its row a wrong input once in every kind */
    PACER_PLANT_TEAMS_EXHAUST,      /* PE 1 claims one split more than it made */
    PACER_PLANT_TEAMS_MEMBER,       /* PE 1 misjudges its membership of one team */
    PACER_PLANT_TEAMS_RING,         /* PE 1 misjudges one context round trip */
    PACER_PLANT_ATOMICS_ADD,        /* PE 1 adds one more than it drew, once */
    PACER_PLANT_ATOMICS_FETCH,      /* PE 1 misreads one value a fetch_add returned */
    PACER_PLANT_ATOMICS_CLAIM,      /* PE 1 believes one claim it never made */
    PACER_PLANT_ATOMICS_SWAP,       /* PE 1 changes one token in hand */
    PACER_PLANT_ATOMICS_SET,        /* PE 1 sets one slot wrong */
    PACER_PLANT_ATOMICS_BITWISE,    /* PE 1 leaves out one xor */
    PACER_PLANT_ATOMICS_FETCHED_BIT, /* PE 1 misjudges one fetched bit */
    PACER_PLANT_RMA_HALO,           /* PE 1 sends one halo byte wrong */
    PACER_PLANT_RMA_PUT,            /* PE 1 puts one element wrong */
    PACER_PLANT_RMA_GAP,            /* PE 1 writes a byte into a strided put's gap */
    PACER_PLANT_RMA_GET,            /* PE 1 changes a byte of what others read */
    PACER_PLANT_RMA_GET_GAP,        /* PE 1 writes a byte into its own get's gap */
    PACER_PLANT_RMA_PTR,            /* PE 1 stores one word wrong through shmem_ptr */
    PACER_PLANT_LOCKS_COUNT,        /* PE 1 counts one acquisition twice */
    PACER_PLANT_LOCKS_HOLDER,       /* PE 1 writes a holder word not its own */
    PACER_PLANT_MEMORY_FREE,        /* PE 1 overwrites a byte of a block it frees */
    PACER_PLANT_MEMORY_CALLOC,      /* PE 1 writes a byte into a fresh calloc */
    PACER_PLANT_MEMORY_ALIGN,       /* PE 1 misreads an aligned address */
    PACER_PLANT_MEMORY_ACCESSIBLE,  /* PE 1 misreads one addr_accessible */
    PACER_PLANT_MEMORY_AGREE,       /* PE 1 misrecords one allocation */
    PACER_PLANT_COLLECTIVES_REDUCE, /* PE 1 gives one reduction a forcing input */
    PACER_PLANT_COLLECTIVES_SCAN,   /* PE 1 gives one scan an input one too many */
    PACER_PLANT_COLLECTIVES_DATA,   /* PE 1 offers one byte wrong to a gather */
    PACER_PLANT_COLLECTIVES_GAP,    /* PE 1 writes a byte into its alltoalls gap */
    PACER_PLANT_ATOMICS_FETCH_COUNT, /* PE 1 increments one slot once more */
    PACER_PLANT_ATOMICS_ATTEMPTS,   /* PE 1 gives up claiming after one claim */
    PACER_PLANT_RMA_FLAG,           /* PE 1 misreads its halo flag once */
    PACER_PLANT_RMA_SINGLE,         /* PE 1 sends one value wrong by p */
    PACER_PLANT_RMA_PTR_NULL,       /* PE 1 takes one shmem_ptr for NULL */
    PACER_PLANT_MEMORY_REALLOC,     /* PE 1 overwrites a byte of a block it reallocs */
    PACER_PLANT_MEMORY_REFUSED,     /* PE 1 overwrites a byte of a block whose realloc is refused */
    PACER_PLANT_COLLECTIVES_STATUS, /* PE 1 misreads one collective's status */
    PACER_PLANT_COLLECTIVES_TIMED,  /* PE 1 gives one timed reduction a wrong input */
    PACER_PLANT_COLLECTIVES_TIMED_DATA, /* PE 1 offers one byte wrong to a timed gather */
    PACER_PLANT_MEMORY_GRANTED,     /* PE 1 takes a refused realloc for granted */
    PACER_PLANT_PRINT_FOLD,         /* PE 1 folds host numbers without leading zeros */
    PACER_PLANT_PRINT_ROUTE,        /* PE 1's record by the signal route changed */
    PACER_PLANT_RMA_SIGNAL,         /* PE 1 misreads one put_signal's signal */
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
    int threads;                    /* worker threads a PE, for the threads exercise */
    int teams_exhaust;              /* the teams exercise splits until a split fails */
    int mix_teams;                  /* mix: the teams the world is split into; 0 chooses */
    double period;                  /* mix: a period's mean length, seconds */
    int mixing;                     /* set by mix: summaries are mix's to print */
    int fold_nodes;                 /* parallel_print labels by host where it can */
    int print_signal;               /* parallel_print by the signal route */
};

/* parallel_print's text capacity, NUL included, as the driver sets it. */
enum { PACER_MESSAGE_CAPACITY = 1024 };

/* The phases an exercise may time, named by its `phases` list. */
enum { PACER_PHASES = 5 };

struct pacer_counters {
    int finished;                   /* nothing more to do: this checkpoint is the last */
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
    int multiple;                   /* needs SHMEM_THREAD_MULTIPLE */
    uint64_t (*memory)(int npes, const struct pacer_options *options, char *fields,
                       size_t size);
    int (*setup)(shmem_team_t team, const struct pacer_options *options);
    void (*step)(long units, int last);
    void (*checkpoint)(struct pacer_counters *counters);
    void (*teardown)(void);
    /* For mix (proposal §4.9), and optional: move the set-up exercise onto
     * `team` for a period. Every member of `team` calls it, once every member
     * of the team it ran on last has ended its last step. Its symmetric
     * memory, sized at setup for the world, is kept; its state starts afresh
     * from `options`, the period's seed among them, so that nothing a
     * previous period left can pass a check - all but its round, which an
     * exercise whose cases cycle carries on, so that short periods still
     * reach every case in turn. Nonzero: it cannot run on `team` - sort and
     * alltoall, for two, run on the world alone. */
    int (*rebind)(shmem_team_t team, const struct pacer_options *options);
};

extern const struct pacer_exercise pacer_sort, pacer_alltoall, pacer_mailbox, pacer_teams,
    pacer_threads, pacer_print, pacer_atomics, pacer_rma, pacer_locks, pacer_memory,
    pacer_collectives;

/*
 * The job's shape, from SHMEM_TEAM_SHARED at start: every world PE's node,
 * nodes numbered in the order of their leads' world ranks (a node's lead is
 * its shared team's rank 0), and its rank on its node. Nodes pair off, 0 with
 * 1, 2 with 3 and so on, and a PE's partner is the PE of its rank on the
 * other node of its pair: -1 where that node, or a PE of that rank, is
 * missing.
 */
struct pacer_layout {
    int pes;
    int nodes;
    const int *node;                /* [pes] */
    const int *local;               /* [pes] */
    const int *partner;             /* [pes] */
};
const struct pacer_layout *pacer_layout(void);

/* Seconds on a monotonic clock. */
double pacer_now(void);

/* What an exercise's own lines start with: nothing, or under mix its team's
 * name and a space, so that a reader knows which team said it. */
const char *pacer_prefix(void);

/*
 * A failure found by this PE, by any of its threads: counted, and the first
 * few lines kept for the next checkpoint, which prints them through
 * parallel_print on the exercise's team and clears them. The text is `exercise round=r what: value`, with what
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
