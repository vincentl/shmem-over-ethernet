/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Cache-line size used for _Alignas on shared words (§3.11, §3.14, §5.3).
 *
 * 128 on aarch64: Apple cores, where the development VM runs, use 128-byte
 * lines even though the guest reports 64, and 128 is a multiple of the 64 used
 * by the Cortex-A72 and A76 in the Pis, so it separates words on both. The
 * cost is padding on a handful of shared words. 64 elsewhere.
 */
#ifndef OSOE_BASE_CACHELINE_H
#define OSOE_BASE_CACHELINE_H

#if defined(__aarch64__)
#define OSOE_CACHELINE 128
#else
#define OSOE_CACHELINE 64
#endif

#endif /* OSOE_BASE_CACHELINE_H */
