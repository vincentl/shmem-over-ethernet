/*
 * A C11 generic called with the wrong number of arguments must not compile
 * (shmem.h: the OSOE_PICKn selections name, in every slot no right call
 * reaches, a function that does not exist and says what the call takes).
 * Before that, such a call selected an empty name and became a parenthesized
 * comma expression, which evaluated its arguments and did nothing: compiled
 * without a word unless -Wall asked about an unused value.
 *
 * shmem.generic-arity.* compile this once per family the selection serves - a
 * nonblocking AMO, a blocking one, an RMA - each with one wrong call, and pass
 * when the compiler's complaint names the function.
 */
#include <shmem.h>

long target, fetched;

void wrong_arity(void);
void wrong_arity(void)
{
#if defined(OSOE_TEST_ARITY_fetch_add_nbi)
    shmem_atomic_fetch_add_nbi(&target, 1L, 0);          /* fetch left out */
#elif defined(OSOE_TEST_ARITY_fetch_add)
    (void)shmem_atomic_fetch_add(&target, 0);            /* value left out */
#elif defined(OSOE_TEST_ARITY_put)
    shmem_put(&target, &fetched, 1);                     /* pe left out */
#endif
}
