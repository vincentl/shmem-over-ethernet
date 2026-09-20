/*
 * The intranode path of the transport boundary (§4.10, §4.1).
 *
 * Not a backend of its own any more. transport-am.c owns §4.10's entry points
 * and sends each operation here or over the AM layer, on the answer the one
 * locality function gives (§8.1); these are what it calls for a PE that
 * osoe_transport_direct accepts.
 *
 * Each is a memory operation on a mapping this process already holds, applied
 * in the calling thread before it returns, so nothing taking this path is ever
 * outstanding and none of them takes a context: there is no ordering state to
 * carry when every operation is already complete. The dirty set belongs to the
 * far half.
 *
 * `pe` here has already been resolved as local. Passing one that has not is a
 * null dereference rather than a slow path, which is deliberate: the locality
 * decision lives in exactly one place (§8.1) and this file is not it.
 */
#ifndef OSOE_TRANSPORT_LOCAL_H
#define OSOE_TRANSPORT_LOCAL_H

#include <stddef.h>
#include <stdint.h>

#include "transport.h"

void osoe_transport_local_put(int pe, enum osoe_region region, uint64_t offset,
                              const void *src, size_t len, unsigned flags);

void osoe_transport_local_get(int pe, enum osoe_region region, uint64_t offset,
                              void *dst, size_t len);

uint64_t osoe_transport_local_rmw(int pe, enum osoe_region region, uint64_t offset,
                                  enum osoe_rmw_op op, unsigned width, uint64_t operand,
                                  uint64_t compare, unsigned flags);

#endif /* OSOE_TRANSPORT_LOCAL_H */
