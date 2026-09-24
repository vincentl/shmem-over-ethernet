/*
 * What the rest of the library needs from the locks (lock.c): a word on
 * stderr when the last shmem_finalize begins with locks still held, since a PE
 * waiting for one never reaches that finalize's barrier; and a session's end
 * forgetting the locks the PE held in it, so that a program that finalized
 * holding one - its error - is not told in its next session that it holds a
 * lock at the same offset already.
 */
#ifndef OSOE_LOCK_H
#define OSOE_LOCK_H

void osoe_lock_warn_held(void);
void osoe_lock_session_end(void);

#endif /* OSOE_LOCK_H */
