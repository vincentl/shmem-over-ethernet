/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Which PID namespace a process is in (D26 of docs/prompts/v0.2-open-items.md).
 *
 * A node's PEs watch each other by process id - a pidfd for a death, the
 * deadlock detector's /proc/<pid>/task, a leftover's recorded creator - and a
 * process id names a process only in its own namespace: in another, the same
 * number names another process or none, so a death goes unseen and a live PE
 * is taken for dead. The start-up check that a node's PEs share one namespace
 * (shmem/src/node.c) and the judges of a leftover (shmem/src/shm-object.c,
 * osoe-stat --gc) compare what this reads.
 *
 * A namespace is named by the device and inode of /proc/self/ns/pid, which
 * stat follows to the namespace itself (namespaces(7): two processes are in
 * one namespace when those are the same). Where /proc cannot say - not
 * mounted, or another namespace's /proc that does not list this process -
 * both are 0, and a comparison with it says nothing.
 */
#ifndef OSOE_BASE_PID_NAMESPACE_H
#define OSOE_BASE_PID_NAMESPACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* As it is laid out in the node's control segment (shmem/src/node.h, layout
 * 42): two 64-bit words, the device first. */
struct osoe_pid_namespace {
    uint64_t device;
    uint64_t inode;
};

/* The calling process's, or zeros where /proc cannot say. */
struct osoe_pid_namespace osoe_pid_namespace_own(void);

/* Whether `n` names a namespace: zeros name none. */
bool osoe_pid_namespace_known(struct osoe_pid_namespace n);

/* Whether `a` and `b` are known to be two namespaces: both known, and not the
 * same. Unknown is never different. */
bool osoe_pid_namespace_differ(struct osoe_pid_namespace a, struct osoe_pid_namespace b);

/* `n` as readlink shows it, "pid:[4026531836]", or "unknown". */
void osoe_pid_namespace_text(struct osoe_pid_namespace n, char *buffer, size_t size);

#endif /* OSOE_BASE_PID_NAMESPACE_H */
