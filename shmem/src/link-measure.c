/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "link-measure.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "blocked.h"
#include "bootstrap.h"
#include "collective-tree.h"
#include "collectives.h"
#include "fatal.h"
#include "internal-region.h"
#include "job.h"
#include "node.h"
#include "state.h"
#include "team.h"
#include "transport.h"

/*
 * How much the root measures. Each number is a median of its tries, which a
 * delayed try - a wake late, a page touched first - cannot move as it moves a
 * mean. On the Pi's gigabit 25 ms in all, most of it the streams (measured,
 * 2026-10-06: 24.8 to 25.3 ms a start); on one machine's loopback a few.
 */
enum {
    HOP_TRIPS = 15,             /* round trips of one small put for h */
    FAN_TRIES = 15,             /* one put to each of the root's first children, for o */
    STREAM_TRIES = 3,
};
static const uint64_t STREAM_SHORT = UINT64_C(128) * 1024;
static const uint64_t STREAM_LONG = UINT64_C(512) * 1024;

static int compare_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t median(uint64_t *values, size_t count)
{
    qsort(values, count, sizeof *values, compare_u64);
    return values[count / 2];
}

static int compare_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

/* The mean of the middle three fifths: an average, which the model's o is,
 * that a try delayed as a whole - or one hurried - does not move. */
static int64_t trimmed_mean(int64_t *values, size_t count)
{
    qsort(values, count, sizeof *values, compare_i64);
    size_t cut = count / 5;
    int64_t sum = 0;
    for (size_t i = cut; i < count - cut; i++)
        sum += values[i];
    return sum / (int64_t)(count - 2 * cut);
}

/*
 * The child's scratch for the measurement: its lanes, which only a large
 * collective's grant lends, and none has begun - every PE is still in
 * shmem_init - so nothing reads them before the first grant rewrites the
 * slots it lends (internal-region.h). `window` is the bytes a stream may
 * cover from `at`, in whole pieces; a longer stream wraps.
 */
struct scratch {
    uint64_t at, window;
};

static struct scratch child_scratch(void)
{
    const struct osoe_bootstrap *b = &osoe.bootstrap;
    uint64_t lanes = (uint64_t)(b->max_threads + 1) *
                     osoe_internal_lane_bytes(b->collective_fanout, b->collective_pipeline);
    uint64_t window = lanes / OSOE_INTERNAL_PIECE_BYTES * OSOE_INTERNAL_PIECE_BYTES;
    return (struct scratch){
        .at = osoe_internal_lanes_offset(b->collective_fanout, b->max_teams,
                                         b->small_collective_bytes, b->node_count),
        .window = window < STREAM_LONG ? window : STREAM_LONG,
    };
}

/* `count` puts of `bytes` each into the child's scratch, then the wait that
 * completes them there: the time of one round trip and of what the puts add. */
static uint64_t timed_puts(struct osoe_transport_context *context, int child,
                           const struct scratch *scratch, const void *data, uint64_t count,
                           uint64_t bytes)
{
    uint64_t start = osoe_now_ns();
    uint64_t offset = 0;
    for (uint64_t i = 0; i < count; i++) {
        if (offset + bytes > scratch->window)
            offset = 0;
        osoe_transport_put(context, child, OSOE_REGION_INTERNAL, scratch->at + offset, data,
                           (size_t)bytes, 0);
        offset += bytes;
    }
    osoe_transport_wait(context, &child, 1, "shmem_init");
    return osoe_now_ns() - start;
}

/* One small put to each of `count` leads, then the wait that completes them:
 * a parent serving its children, as a tree's level does. */
static uint64_t timed_fan(struct osoe_transport_context *context, const int *leads,
                          uint32_t count, const struct scratch *scratch, const void *data)
{
    uint64_t start = osoe_now_ns();
    for (uint32_t i = 0; i < count; i++)
        osoe_transport_put(context, leads[i], OSOE_REGION_INTERNAL, scratch->at, data, 8, 0);
    osoe_transport_wait(context, leads, count, "shmem_init");
    return osoe_now_ns() - start;
}

/*
 * The root's lead against its first child's (the brief's "Measuring the
 * link"): h from round trips of one small put and its completion, halved;
 * B as the slope of a long stream of pieces over a short one, so that the
 * hop cancels. And o as the model means it - what a parent spends on each
 * child it serves before it can release - from one put to each of the root's
 * first `fan` children against one put alone, slope over the extra children:
 * a burst to one child would not do, since a connection carries consecutive
 * small messages together. A job of two nodes has no second child and no
 * choice that o could move - every fan-out is the same tree there - and
 * measures o as 0. Integers in nanoseconds and bytes, as the chooser takes
 * them.
 */
static struct osoe_link measure(const int *children, uint32_t fan)
{
    int child = children[0];
    struct osoe_transport_context *context = osoe_transport_context_default();
    struct scratch scratch = child_scratch();
    unsigned char *piece = calloc(1, OSOE_INTERNAL_PIECE_BYTES);
    if (!piece)
        osoe_fatal("out of memory for the link's measurement");
    const uint64_t word = 8;

    /* Not timed: a child's lead may still be starting its agent, which its
     * first completion waits for. */
    for (uint32_t i = 0; i < fan; i++)
        timed_puts(context, children[i], &scratch, piece, 1, word);

    uint64_t trips[HOP_TRIPS];
    for (unsigned i = 0; i < HOP_TRIPS; i++)
        trips[i] = timed_puts(context, child, &scratch, piece, 1, word);
    uint64_t trip = median(trips, HOP_TRIPS);

    /* Paired: each try's fan against the single put just before it, so that
     * a slow stretch of the run moves both. The extra a fan costs comes as a
     * few tries some 20 us late among many that cost nothing more (the Pi,
     * 2026-10-06), which a median reads as 0 or 20 by turns; the model's o is
     * the average a level pays, so the trimmed mean. */
    int64_t extra = 0;
    if (fan > 1) {
        int64_t differences[FAN_TRIES];
        for (unsigned i = 0; i < FAN_TRIES; i++) {
            uint64_t alone = timed_fan(context, children, 1, &scratch, piece);
            uint64_t served = timed_fan(context, children, fan, &scratch, piece);
            differences[i] = (int64_t)served - (int64_t)alone;
        }
        extra = trimmed_mean(differences, FAN_TRIES);
    }

    uint64_t short_times[STREAM_TRIES], long_times[STREAM_TRIES];
    uint64_t piece_bytes = OSOE_INTERNAL_PIECE_BYTES;
    for (unsigned i = 0; i < STREAM_TRIES; i++) {
        short_times[i] = timed_puts(context, child, &scratch, piece,
                                    STREAM_SHORT / piece_bytes, piece_bytes);
        long_times[i] = timed_puts(context, child, &scratch, piece,
                                   STREAM_LONG / piece_bytes, piece_bytes);
    }
    uint64_t short_time = median(short_times, STREAM_TRIES);
    uint64_t long_time = median(long_times, STREAM_TRIES);
    free(piece);

    struct osoe_link link;
    link.hop_ns = trip / 2 ? trip / 2 : 1;
    link.message_ns = extra > 0 ? (uint64_t)extra / (fan - 1) : 0;
    if (link.message_ns > OSOE_LINK_MESSAGE_NS_HIGHEST)
        link.message_ns = OSOE_LINK_MESSAGE_NS_HIGHEST;
    /* The stream's slope; a long stream no slower than the short one - a
     * link faster than the clock can tell apart - is the highest a setting
     * may give. */
    uint64_t longer = STREAM_LONG - STREAM_SHORT;
    link.bytes_per_second = long_time > short_time
                                ? longer * UINT64_C(1000000000) / (long_time - short_time)
                                : OSOE_LINK_BYTES_PER_SECOND_HIGHEST;
    if (link.bytes_per_second == 0)
        link.bytes_per_second = 1;
    if (link.bytes_per_second > OSOE_LINK_BYTES_PER_SECOND_HIGHEST)
        link.bytes_per_second = OSOE_LINK_BYTES_PER_SECOND_HIGHEST;
    if (link.hop_ns > OSOE_LINK_HOP_NS_HIGHEST)
        link.hop_ns = OSOE_LINK_HOP_NS_HIGHEST;
    return link;
}

void osoe_link_establish(void)
{
    const struct osoe_bootstrap *b = &osoe.bootstrap;
    struct osoe_link *in_force = &osoe.job.link;
    *in_force = (struct osoe_link){ 0 };
    osoe.job.link_source = OSOE_LINK_NONE;
    osoe.job.link_ns = 0;
    if (osoe.job.node_count < 2 || b->collective_shape == OSOE_COLLECTIVE_SHAPE_FIXED)
        return;
    if (b->link_bytes_per_second) {
        *in_force = (struct osoe_link){ b->link_hop_ns, b->link_message_ns,
                                        b->link_bytes_per_second };
        osoe.job.link_source = OSOE_LINK_SET;
        return;
    }

    /*
     * The root of every team's tree over the job's nodes is node 0, its first
     * child node 1 (collective-tree.c, level order). The root's lead measures;
     * every PE then takes part in an AND over the finalize team's split words
     * (collectives.h) - the root's numbers against every other PE's all ones,
     * so that each PE ends holding the root's. The finalize team's counters,
     * not the world's: no application collective touches them, so the world's
     * epochs a user reads in osoe-stat stay the application's own.
     */
    uint64_t start = osoe_now_ns();
    struct osoe_team *finalize = osoe_team_finalize();
    uint64_t *words = osoe_team_split_words(finalize);
    uint32_t root = osoe_job_node_lead(&osoe.job, 0);
    if (osoe.bootstrap.rank == root) {
        /* The root's first children: nodes 1 to the ceiling, as many as the
         * job has. */
        int children[OSOE_COLLECTIVE_FANOUT_MAX];
        uint32_t fan = osoe.job.node_count - 1;
        if (fan > b->collective_fanout)
            fan = b->collective_fanout;
        for (uint32_t i = 0; i < fan; i++)
            children[i] = (int)osoe_job_node_lead(&osoe.job, i + 1);
        struct osoe_link measured = measure(children, fan);
        words[0] = measured.hop_ns;
        words[1] = measured.message_ns;
        words[2] = measured.bytes_per_second;
    } else {
        words[0] = words[1] = words[2] = UINT64_MAX;
    }
    osoe_team_split_and("shmem_init", finalize, 3);
    *in_force = (struct osoe_link){ words[0], words[1], words[2] };
    osoe.job.link_source = OSOE_LINK_MEASURED;
    osoe.job.link_ns = osoe_now_ns() - start;
    /* Release, as every store here names one (§3.16): three numbers for
     * osoe-stat to show, which nothing in the library reads back. */
    if (osoe.bootstrap.local_id == 0) {
        struct osoe_node_header *h = osoe.node.hdr;
        atomic_store_explicit(&h->measured_link_hop_ns, in_force->hop_ns, memory_order_release);
        atomic_store_explicit(&h->measured_link_message_ns, in_force->message_ns,
                              memory_order_release);
        atomic_store_explicit(&h->measured_link_bytes_per_second, in_force->bytes_per_second,
                              memory_order_release);
    }
}
