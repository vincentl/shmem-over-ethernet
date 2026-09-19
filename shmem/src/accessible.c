/*
 * Accessibility queries (spec §9.1.7, §9.1.8).
 *
 * Both answer the same underlying question - can this PE reach that object on
 * that PE through OpenSHMEM - and both are answered from what the library
 * already knows, not from a new mechanism.
 *
 * `shmem_pe_accessible` asks whether the remote PE runs the same executable,
 * which is what makes full support for symmetric objects available. Every PE of
 * an osoe job does, by construction: the launcher starts one program and §6.2's
 * rendezvous refuses a job whose PEs disagree about anything. So the answer is
 * whether `pe` names a PE of this job at all.
 *
 * `shmem_addr_accessible` additionally asks whether `addr` is a symmetric
 * object. Symmetric statics are not supported until §4.3's phase 1.5, so an
 * address outside the heap is not accessible and says so - which §7.4 records
 * as the known non-conformance, and which is a far better answer than 1
 * followed by a fault at the first put.
 */
#include <stdint.h>

#include <pshmem.h>
#include <shmem.h>

#include "export.h"
#include "state.h"
#include "symmetric.h"

OSOE_API int pshmem_pe_accessible(int pe)
{
    return pe >= 0 && (uint32_t)pe < osoe.bootstrap.world_size;
}

OSOE_API int pshmem_addr_accessible(const void *addr, int pe)
{
    uint64_t offset;
    return pshmem_pe_accessible(pe) && osoe_symmetric_heap_offset(addr, 1, &offset);
}

/* The weak `shmem_` aliases (export.h). */
OSOE_ALIAS(pe_accessible)
OSOE_ALIAS(addr_accessible)
