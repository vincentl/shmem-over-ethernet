/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * osoe-stat's side of test-blocked-record.c: a segment whose metric table
 * describes the library's PE slot, and the tool's reader of the record over
 * it. Its own file because the tool's segment.h and the library's node.h each
 * declare the segment's directory, and a reader built from segment.h alone is
 * the point: the tool knows the record only through the table.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "segment.h"
#include "test-blocked-record.h"

static uint32_t fnv1a(const void *p, size_t n)
{
    const unsigned char *b = p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}

static struct osoe_metric_descriptor descriptor(const char *name, unsigned type, size_t width,
                                                size_t offset)
{
    struct osoe_metric_descriptor m;
    memset(&m, 0, sizeof m);
    snprintf(m.name, sizeof m.name, "%s", name);
    m.type = (uint16_t)type;
    m.kind = OSOE_MKIND_STATE;
    m.scope = OSOE_MSCOPE_PE;
    m.width = (uint16_t)width;
    m.offset = (uint32_t)offset;
    return m;
}

unsigned char *blocked_segment_build(const char *path, const struct blocked_record_layout *l,
                                     size_t *bytes)
{
    struct osoe_metric_descriptor table[] = {
        descriptor("pe.blocked.gen", OSOE_MTYPE_U64, 8, l->gen),
        descriptor("pe.blocked.routine", OSOE_MTYPE_STRING, l->routine_bytes, l->routine),
        descriptor("pe.blocked.detail", OSOE_MTYPE_U64, 8, l->detail),
        descriptor("pe.blocked.detail_kind", OSOE_MTYPE_U32, 4, l->kind),
        descriptor("pe.blocked.since_ns", OSOE_MTYPE_U64, 8, l->since),
    };
    uint32_t n = (uint32_t)(sizeof table / sizeof table[0]);
    uint64_t slots_offset = BLOCKED_SEGMENT_HEADER_BYTES;
    uint64_t metrics_offset = slots_offset + l->slot_bytes;
    uint64_t blocks_offset = (metrics_offset + (uint64_t)n * sizeof table[0] + 63) & ~(uint64_t)63;
    uint64_t total = blocks_offset + 64;

    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 || ftruncate(fd, (off_t)total) != 0) {
        perror(path);
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    unsigned char *p = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    struct osoe_segment_directory d;
    memset(&d, 0, sizeof d);
    d.magic = OSOE_SEGMENT_MAGIC;
    d.layout = l->layout;
    d.directory_bytes = (uint32_t)sizeof d;
    d.header_bytes = BLOCKED_SEGMENT_HEADER_BYTES;
    d.segment_bytes = total;
    d.slots_offset = slots_offset;
    d.slot_bytes = (uint32_t)l->slot_bytes;
    d.local_count = 1;
    d.metrics_offset = metrics_offset;
    d.metrics_bytes = (uint64_t)n * sizeof table[0];
    d.metrics_count = n;
    d.metric_descriptor_bytes = (uint32_t)sizeof table[0];
    d.metrics_checksum = fnv1a(table, (size_t)d.metrics_bytes);
    d.blocks_offset = blocks_offset;
    d.blocks_bytes = 64;
    d.block_count = 1;
    d.block_bytes = 64;
    d.blocks_per_pe = 1;
    memcpy(p, &d, sizeof d);
    memcpy(p + metrics_offset, table, (size_t)d.metrics_bytes);
    *bytes = total;
    return p;
}

void *blocked_segment_open(const char *path)
{
    struct osoe_segment *s = calloc(1, sizeof *s);
    char err[256];
    if (s && osoe_segment_open(s, path, err, sizeof err) != 0) {
        fprintf(stderr, "osoe_segment_open: %s\n", err);
        free(s);
        return NULL;
    }
    return s;
}

bool blocked_segment_read(void *segment, struct blocked_record_copy *copy)
{
    struct osoe_segment_blocked b;
    if (!osoe_segment_blocked(segment, 0, &b) || !b.valid)
        return false;
    snprintf(copy->routine, sizeof copy->routine, "%s", b.routine);
    copy->kind = b.detail_kind;
    copy->detail = b.detail;
    copy->since_ns = b.since_ns;
    return true;
}

void blocked_segment_close(void *segment)
{
    osoe_segment_close(segment);
    free(segment);
}
