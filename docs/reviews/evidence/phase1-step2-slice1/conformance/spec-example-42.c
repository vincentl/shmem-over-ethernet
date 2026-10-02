/* OpenSHMEM 1.6 Example 42 (section 9.11.8), verbatim apart from this comment and the notice at the end. */
#include <shmem.h>
#include <stdio.h>
int user_wait_any(long *ivar, int count, int cmp, long value) {
  int idx = 0;
  while (!shmem_test(&ivar[idx], cmp, value))
    idx = (idx + 1) % count;
  return idx;
}
int main(void) {
  shmem_init();
  const int mype = shmem_my_pe();
  const int npes = shmem_n_pes();
    long *wait_vars = shmem_calloc(npes, sizeof(long));
    if (mype == 0) {
      int who = user_wait_any(wait_vars, npes, SHMEM_CMP_NE, 0);
      printf("PE %d observed first update from PE %d\n", mype, who);
    }
    else
      shmem_atomic_set(&wait_vars[mype], mype, 0);
    shmem_free(wait_vars);
    shmem_finalize();
    return 0;
}

/*
 * Notice. This program is Example 42 (section 9.11.8) of "OpenSHMEM Application
 * Programming Interface, Version 1.6" (1 November 2024), copied from it. The
 * specification belongs to Open Source Software Solutions, Inc. (OSSS), a
 * nonprofit organization, under an agreement with HPE, and permits copying all
 * or part of it without fee provided the OSSS notice and the document's title
 * appear and notice is given that the copying is by permission of OSSS. This
 * copy is made by permission of OSSS. The program is the specification's, not
 * osoe's, and so has no SPDX line.
 */
