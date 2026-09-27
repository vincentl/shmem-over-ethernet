/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * parallel_print - every member of a team formats its own text, and the team's
 * rank 0 prints each distinct text once, under the PEs that said it
 * (docs/design/osoe-pacer-proposal.md §2):
 *
 *   [0-15] round 7: 16.8 M keys exchanged
 *   [0-3,8-11] 112 MiB/s
 *   [4-7,12-15] 94 MiB/s
 *   even[0,2,4,6] alltoall block=4096 collective_us=612
 *
 * The bracket holds the group's world PE numbers, sorted, folded as Slurm
 * folds a host list: runs as a-b, joined by commas. A call on the world team
 * has no prefix; on SHMEM_TEAM_SHARED the prefix is `shared`; on a team given
 * a name here, that name; on any other, `team(start:stride:size)` in world
 * ranks, which every member computes alike. Lines come in the order of each
 * group's lowest team rank. An empty text is silence, and a group of silent
 * PEs prints nothing, so a subset can report.
 *
 * Folded to nodes (parallel_print_fold_nodes), a group that holds every PE
 * of each host it touches is labeled by the hosts' names instead, folded the
 * same way, and a team's name comes before them with an `@`:
 *
 *   pi-[001-004] round 7: 16.8 M keys exchanged
 *   shared@pi-003 94 MiB/s
 *   [0-2,4-15] connection to pi-004 lost
 *
 * A host is a host name, as gethostname gives it up to its first dot: nodes
 * that share one - osoe-launch's logical nodes on one machine - fold together.
 *
 * Written against the OpenSHMEM API alone, so that it builds against any
 * implementation: a failure of the tool can be told from one of the library
 * by building the same program there.
 */
#ifndef PACER_PARALLEL_PRINT_H
#define PACER_PARALLEL_PRINT_H

#include <stddef.h>

#include <shmem.h>

/* Collective on SHMEM_TEAM_WORLD, once, after shmem_init: allocates 16 bytes
 * a PE for the world, a 64 KiB arena and a private buffer of `capacity`
 * bytes, and gathers every PE's host name. A text longer than capacity - 1
 * bytes is cut to end in "...". 0 on success. */
int parallel_print_init(size_t capacity);

/* Local: the name a team's lines carry, at most 31 characters; NULL forgets
 * the team, which a program should do before destroying it. Every member must
 * give the same name. 0, or -1 when 64 teams already have names. */
int parallel_print_name_team(shmem_team_t team, const char *name);

/* Collective on `team`: every member calls it, with its own text. Returns the
 * number of distinct lines printed, on every member; -1 before init, and on
 * SHMEM_TEAM_INVALID without taking part, so a PE outside a team may call it
 * alike. One fcollect (or the signal route's rounds), a put from each distinct
 * text's owner, one sync. Its quiet also completes the calling PE's earlier
 * puts on the default context. */
int parallel_print(shmem_team_t team, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

/* Collective on SHMEM_TEAM_WORLD. */
void parallel_print_finalize(void);

/* A world PE's host name as gathered at init, up to its first dot. */
const char *parallel_print_host(int pe);

/* The routes by which every member learns every member's record (proposal
 * §2.5): the library's fcollect, the default, or dissemination by
 * shmem_putmem_signal and waits on the signals, which the tool writes itself.
 * The same lines either way, so a run that prints by both and compares them
 * tests the one against the other. Local; every member of a team must choose
 * the same. Returns the previous route. */
enum parallel_print_route { PARALLEL_PRINT_FCOLLECT, PARALLEL_PRINT_SIGNAL };
enum parallel_print_route parallel_print_route(enum parallel_print_route route);

/* Local: 1 labels by host wherever a group holds every PE of each host it
 * touches, and the names fit where the PE numbers would have; 0, the default,
 * labels by PE numbers alone. Only a team's rank 0 writes labels, so a program
 * sets it alike on every PE. Returns the previous setting. */
int parallel_print_fold_nodes(int on);

/* Local: `count` host names folded as Slurm folds a host list - a run of
 * numbers under one prefix and width into brackets, duplicates dropped, the
 * prefixes in order: pi-[001-002,004],login. Written into out as snprintf
 * writes, cut to size; returns the length the whole list needs. */
size_t parallel_print_fold_names(const char *const *names, int count, char *out, size_t size);

/*
 * For osoe-pacer's print exercise and its planted defect alone.
 *
 * _window: the bytes of the arena one window may fill, from 16 to the whole
 * 64 KiB (the default); a text is cut to fit one. Every member of a team must
 * set the same. Returns the previous value.
 *
 * _capture: while `buffer` is not NULL, a team's rank 0 writes each call's
 * lines there, from its start and NUL-terminated, instead of printing them.
 *
 * _plant_hash: this PE hashes its text and one byte more, so its line parts
 * from those of every PE that said the same (osoe-pacer --plant print-hash).
 *
 * _plant_fold: this PE's parallel_print_fold_names writes numbers without
 * their leading zeros (osoe-pacer --plant print-fold).
 *
 * _plant_route: this PE's record, taken by the signal route alone, has its
 * hash changed - alike for every member, which all read it from this PE - so
 * that route's lines part where the fcollect's do not (--plant print-route).
 */
size_t parallel_print_window(size_t bytes);
void parallel_print_capture(char *buffer, size_t size);
void parallel_print_plant_hash(int on);
void parallel_print_plant_fold(int on);
void parallel_print_plant_route(int on);

#endif /* PACER_PARALLEL_PRINT_H */
