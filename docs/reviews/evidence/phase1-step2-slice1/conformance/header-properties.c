/* Spec section 6: library constants are compile-time constants (usable as
 * array sizes and case labels). Section 7: handles are at least link-time
 * constants, usable "in initialization expressions or assignments".
 * Section 9.2: the thread levels increase. Compiled as C99, C11 and C++. */
#include <shmem.h>
#include <shmemx.h>
#include <pshmem.h>
#include <mpp/shmem.h>

static long psync[SHMEM_BARRIER_SYNC_SIZE > SHMEM_SYNC_SIZE ? SHMEM_BARRIER_SYNC_SIZE : SHMEM_SYNC_SIZE];
static double work[SHMEM_REDUCE_MIN_WRKDATA_SIZE];
static char name[SHMEM_MAX_NAME_LEN] = SHMEM_VENDOR_STRING;
static shmem_team_t world = SHMEM_TEAM_WORLD, shared = SHMEM_TEAM_SHARED, none = SHMEM_TEAM_INVALID;
static shmem_ctx_t context = SHMEM_CTX_DEFAULT, no_context = SHMEM_CTX_INVALID;

typedef char thread_levels_increase[(SHMEM_THREAD_SINGLE < SHMEM_THREAD_FUNNELED &&
    SHMEM_THREAD_FUNNELED < SHMEM_THREAD_SERIALIZED &&
    SHMEM_THREAD_SERIALIZED < SHMEM_THREAD_MULTIPLE) ? 1 : -1];
typedef char versions[(SHMEM_MAJOR_VERSION == 1 && SHMEM_MINOR_VERSION == 6 &&
    _SHMEM_MAJOR_VERSION == 1 && _SHMEM_MINOR_VERSION == 6) ? 1 : -1];
typedef char options_are_distinct_bits[((SHMEM_CTX_SERIALIZED & SHMEM_CTX_PRIVATE) == 0 &&
    (SHMEM_CTX_PRIVATE & SHMEM_CTX_NOSTORE) == 0 && (SHMEM_CTX_SERIALIZED & SHMEM_CTX_NOSTORE) == 0 &&
    (SHMEM_MALLOC_ATOMICS_REMOTE & SHMEM_MALLOC_SIGNAL_REMOTE) == 0) ? 1 : -1];

static int comparison(int cmp)
{
    switch (cmp) {
    case SHMEM_CMP_EQ: case SHMEM_CMP_NE: case SHMEM_CMP_GT:
    case SHMEM_CMP_GE: case SHMEM_CMP_LT: case SHMEM_CMP_LE: return 1;
    case SHMEM_SIGNAL_SET + 100: case SHMEM_SIGNAL_ADD + 100: return 2;
    default: return 0;
    }
}

int main(void)
{
    (void)psync; (void)work; (void)name; (void)world; (void)shared; (void)none;
    (void)context; (void)no_context;
    return comparison(SHMEM_CMP_EQ) == 1 && world != shared && none != world ? 0 : 1;
}
