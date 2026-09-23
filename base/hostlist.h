/*
 * Slurm's compressed host lists (§6.1, §6.2): SLURM_STEP_NODELIST, or its
 * OSOE_STEP_NODELIST mirror.
 *
 *     pi[01-04,07],login,rack[1-2]-a
 *
 * names pi01 pi02 pi03 pi04 pi07 login rack1-a rack2-a, in that order, which is
 * the order SLURM_NODEID indexes. Entries are separated by commas outside
 * brackets; an entry is a prefix, at most one bracketed list of numbers and
 * ranges, and a suffix. A range keeps the width of its lower bound, so 01-04
 * gives 01 02 03 04. Slurm also writes several bracket groups in one entry for
 * multi-dimensional names; that form is refused by name rather than guessed.
 *
 * Pure string handling, and written with tier-1 tests (§6.2) because it would
 * otherwise first be debugged on a cluster. A lead needs only a few entries -
 * its own and its rendezvous parent's - so nothing here expands the list: each
 * call walks the string, O(its length).
 */
#ifndef OSOE_BASE_HOSTLIST_H
#define OSOE_BASE_HOSTLIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * How many hosts `list` names. Returns false if it is not a host list, with a
 * short reason in `why` (why_len bytes).
 */
bool osoe_hostlist_count(const char *list, uint32_t *count, char *why, size_t why_len);

/*
 * The `index`th host of `list`, counting from 0, into `out` (out_len bytes).
 * False if `list` is not a host list, `index` is past its end, or the name does
 * not fit.
 */
bool osoe_hostlist_entry(const char *list, uint32_t index, char *out, size_t out_len);

#endif /* OSOE_BASE_HOSTLIST_H */
