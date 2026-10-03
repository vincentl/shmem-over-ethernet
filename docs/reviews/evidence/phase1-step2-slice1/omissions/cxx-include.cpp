// SPDX-License-Identifier: LGPL-3.0-or-later
// Does a C++ translation unit survive every public header, and what does a
// C++ caller actually get?
#include <shmem.h>
#include <pshmem.h>
#include <shmemx.h>
#include <mpp/shmem.h>
#include <cstdio>

static shmem_team_t world = SHMEM_TEAM_WORLD;      // spec §7: usable in initializers
static shmem_ctx_t  dflt  = SHMEM_CTX_DEFAULT;

int main() {
    shmem_init();
    long *x = static_cast<long *>(shmem_malloc(sizeof(long)));
    *x = 0;
    shmem_long_atomic_add(x, 1, shmem_my_pe());
    shmem_wait_until(x, SHMEM_CMP_EQ, 1L);          // C++: only the deprecated long form
    int rc = shmem_sync(world);                     // macro alias, visible to C++
    std::printf("cxx ok %d %p\n", rc, static_cast<void *>(dflt));
    shmem_free(x);
    shmem_finalize();
    return 0;
}
