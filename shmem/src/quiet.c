/*
 * Memory ordering: shmem_fence, shmem_quiet and shmem_pe_quiet
 * (spec §9.12.1-§9.12.3; design §4.6).
 *
 * Each is one call to the boundary, which is the point of §4.10 having a fence
 * and a wait rather than these three routines: fence orders delivery, wait
 * completes. On the intranode backend both are the serializing store, because
 * every operation was applied in the calling thread before its call returned
 * and nothing is ever outstanding. Against the AM backend the same two calls
 * seal buffers and wait for acknowledgements. Nothing here changes.
 *
 * The context forms (shmem_ctx_fence and the rest) arrive with §7.3's
 * plumbing; these are the default-context spellings, and the split is already
 * how rma.c is written.
 */
#include <stddef.h>

#include <shmem.h>

#include "export.h"
#include "metrics.h"
#include "sym.h"
#include "transport.h"

OSOE_API void shmem_fence(void)
{
    osoe_count(OSOE_C_SHMEM_FENCE_CALLS, 1);
    osoe_tp_fence(osoe_tp_ctx_default());
}

OSOE_API void shmem_quiet(void)
{
    osoe_count(OSOE_C_SHMEM_QUIET_CALLS, 1);
    osoe_tp_wait(osoe_tp_ctx_default(), NULL, 0);
}

/*
 * spec §9.12.3: with npes == 0, target_pes is ignored and the routine returns
 * immediately - so a null pointer with npes == 0 is a conforming call and must
 * not be dereferenced, which is why the length is tested first.
 *
 * The boundary distinguishes "every PE" from "these PEs" by the pointer, not
 * the count (osoe_tp_wait with pes == NULL means every PE), so passing an empty
 * list through would be wrong in the other direction: it would wait on nothing
 * rather than on everything only if the backend reads npes first. Returning
 * here says it once, where the spec says it.
 */
OSOE_API void shmem_pe_quiet(const int *target_pes, size_t npes)
{
    if (npes == 0)
        return;
    osoe_count(OSOE_C_SHMEM_PE_QUIET_CALLS, 1);
    for (size_t i = 0; i < npes; i++)
        osoe_check_pe("shmem_pe_quiet", target_pes[i]);
    osoe_tp_wait(osoe_tp_ctx_default(), target_pes, npes);
}
