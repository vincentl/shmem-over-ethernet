/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Between test-blocked-record.c, which sees the library's node.h, and
 * test-blocked-record-stat.c, which sees only osoe-stat's segment.h. */
#ifndef OSOE_TEST_BLOCKED_RECORD_H
#define OSOE_TEST_BLOCKED_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { BLOCKED_SEGMENT_HEADER_BYTES = 4096 };

/* Where the library's PE slot keeps the record, by offsetof. */
struct blocked_record_layout {
    uint32_t layout;
    size_t slot_bytes, gen, routine, routine_bytes, detail, kind, since;
};

struct blocked_record_copy {
    char routine[64];
    uint32_t kind;
    uint64_t detail, since_ns;
};

/* A segment of one PE slot at BLOCKED_SEGMENT_HEADER_BYTES, its table
 * describing the record where `layout` says; the caller writes the slot. */
unsigned char *blocked_segment_build(const char *path, const struct blocked_record_layout *layout,
                                     size_t *bytes);
void *blocked_segment_open(const char *path);
bool blocked_segment_read(void *segment, struct blocked_record_copy *copy);
void blocked_segment_close(void *segment);

#endif /* OSOE_TEST_BLOCKED_RECORD_H */
