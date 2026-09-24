/*
 * What the rest of the library needs from the locks (lock.c): only that a
 * session's end forgets the locks the PE held in it, so that a program that
 * finalized holding one - its error - is not told in its next session that it
 * holds a lock at the same offset already.
 */
#ifndef OSOE_LOCK_H
#define OSOE_LOCK_H

void osoe_lock_session_end(void);

#endif /* OSOE_LOCK_H */
