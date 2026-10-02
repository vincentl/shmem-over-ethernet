/* OpenSHMEM 1.6 Example 38 (section 9.11.2), verbatim apart from this comment and the notice at the end. */
#include <shmem.h>
int main(void) {
  shmem_init();
  int mype = shmem_my_pe();
  int npes = shmem_n_pes();
    int *flags = shmem_calloc(npes, sizeof(int));
    int *status = NULL;
    for (int i = 0; i < npes; i++)
      shmem_atomic_set(&flags[mype], 1, i);
    shmem_wait_until_all(flags, npes, status, SHMEM_CMP_EQ, 1);
    shmem_free(flags);
    shmem_finalize();
    return 0;
}

/*
 * Notice. This program is Example 38 (section 9.11.2) of "OpenSHMEM Application
 * Programming Interface, Version 1.6" (1 November 2024), copied from it. The
 * specification belongs to Open Source Software Solutions, Inc. (OSSS), a
 * nonprofit organization, under an agreement with HPE, and permits copying all
 * or part of it without fee provided the OSSS notice and the document's title
 * appear and notice is given that the copying is by permission of OSSS. This
 * copy is made by permission of OSSS. The program is the specification's, not
 * osoe's, and so has no SPDX line.
 */
