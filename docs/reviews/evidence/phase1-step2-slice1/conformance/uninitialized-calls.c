/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Annex C makes every call into an uninitialized library undefined, and gives
 * "may abort immediately" as the example. Design section 3.15 promises loud
 * failure. This calls one routine before shmem_init (or after the last
 * shmem_finalize with a leading '+') to see which it gets. */
#include <shmem.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "";
    if (*what == '+') {
        what++;
        shmem_init();
        shmem_finalize();
    }
    long local = 0;
    char name[SHMEM_MAX_NAME_LEN];
    int flag;
    if (!strcmp(what, "my_pe"))              printf("my_pe=%d n_pes=%d\n", shmem_my_pe(), shmem_n_pes());
    else if (!strcmp(what, "malloc"))        printf("malloc=%p\n", shmem_malloc(64));
    else if (!strcmp(what, "free"))          shmem_free(&local);
    else if (!strcmp(what, "putmem"))        shmem_putmem(&local, &local, sizeof local, 0);
    else if (!strcmp(what, "atomic"))        shmem_long_atomic_inc(&local, 0);
    else if (!strcmp(what, "wait"))          shmem_long_wait_until(&local, SHMEM_CMP_EQ, 0);
    else if (!strcmp(what, "barrier_all"))   shmem_barrier_all();
    else if (!strcmp(what, "quiet"))         shmem_quiet();
    else if (!strcmp(what, "team_my_pe"))    printf("team_my_pe=%d\n", shmem_team_my_pe(SHMEM_TEAM_WORLD));
    else if (!strcmp(what, "team_sync"))     shmem_team_sync(SHMEM_TEAM_SHARED);
    else if (!strcmp(what, "ptr"))           printf("ptr=%p\n", shmem_ptr(&local, 0));
    else if (!strcmp(what, "pe_accessible")) printf("pe_accessible=%d\n", shmem_pe_accessible(0));
    else if (!strcmp(what, "finalize"))      shmem_finalize();
    else if (!strcmp(what, "info")) { shmem_info_get_name(name); printf("name=%s\n", name); }
    else if (!strcmp(what, "query")) { shmem_query_initialized(&flag); printf("initialized=%d\n", flag);
                                       shmem_query_thread(&flag); printf("thread=%d\n", flag); }
    printf("returned normally\n");
    return 0;
}
