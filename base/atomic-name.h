/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A short name kept in shared memory for another process to read while its
 * writer may be writing the next one: the bytes of a NUL-padded character
 * array, stored and loaded as 64-bit atomic words.
 *
 * Why not the array itself: a reader that copies plain characters between two
 * loads of a generation word (the "blocked in" record, §3.14) may have those
 * plain loads satisfied after its second load of the generation - on aarch64
 * nothing orders them - and accept a name from a later publication beside
 * fields from an earlier one (the round-three review's K1, measured). Release
 * stores and acquire loads of the words order the name as the record's other
 * fields are ordered, with nothing §3.16 does not already allow.
 *
 * The words hold the array's own bytes, in its place: the writer copies the
 * characters into each word and the reader copies them out, so a reader that
 * copies the bytes - a tool built before the words - still finds the name.
 *
 * Header only, and needing nothing but the C library: the library's writer and
 * reader and osoe-stat's reader all include it, so the three cannot disagree on
 * how a name is laid in its words.
 */
#ifndef OSOE_BASE_ATOMIC_NAME_H
#define OSOE_BASE_ATOMIC_NAME_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Store `name` in `count` words, each with release: at most `count` * 8 - 1 of
 * its characters, the rest of the words NUL, so the last byte is always NUL.
 * A NULL name stores the empty name. */
static inline void osoe_atomic_name_store(_Atomic uint64_t *words, size_t count,
                                          const char *name)
{
    size_t length = 0;
    while (name && length + 1 < count * 8 && name[length] != '\0')
        length++;
    for (size_t i = 0; i < count; i++) {
        char chunk[8] = { 0 };
        size_t from = i * 8;
        if (from < length)
            memcpy(chunk, name + from, length - from < 8 ? length - from : 8);
        uint64_t word;
        memcpy(&word, chunk, sizeof word);
        atomic_store_explicit(&words[i], word, memory_order_release);
    }
}

/* Load the name in `count` words, each with acquire, into `out` of `size`
 * bytes: as many of its bytes as fit before a closing NUL, which is always
 * written. Whole words are copied by a fixed size and the last one byte by
 * byte, so that a fixed-size name compiles to its loads in a row and nothing
 * else - the shape in which a plain load was seen to pass the generation's. */
static inline void osoe_atomic_name_load(const _Atomic uint64_t *words, size_t count,
                                         char *out, size_t size)
{
    if (size == 0)
        return;
    size_t want = count * 8 < size - 1 ? count * 8 : size - 1;
    size_t i = 0;
    for (; (i + 1) * 8 <= want; i++) {
        uint64_t word = atomic_load_explicit(&words[i], memory_order_acquire);
        memcpy(out + i * 8, &word, sizeof word);
    }
    if (i * 8 < want) {
        uint64_t word = atomic_load_explicit(&words[i], memory_order_acquire);
        const char *bytes = (const char *)&word;
        for (size_t b = 0; i * 8 + b < want; b++)
            out[i * 8 + b] = bytes[b];
    }
    out[want] = '\0';
}

#endif /* OSOE_BASE_ATOMIC_NAME_H */
