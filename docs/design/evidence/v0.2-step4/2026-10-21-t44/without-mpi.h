/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * T44: stand-ins for the few MPI names mpi-and-shmem.c uses, so that the same
 * program builds without Open MPI (-DT44_WITHOUT_MPI): the baseline for what
 * linking Open MPI changes in a process that runs osoe alone. Any call ends the
 * program; the program makes none unless it is asked to start MPI.
 */
#ifndef T44_WITHOUT_MPI_H
#define T44_WITHOUT_MPI_H
#include <stdio.h>
#include <stdlib.h>

typedef int MPI_Comm;
typedef int MPI_Datatype;
typedef int MPI_Op;
typedef int MPI_Status;
typedef int MPI_Win;
typedef int MPI_Info;
#define MPI_INFO_NULL 0
#define MPI_COMM_WORLD 0
#define MPI_INT 0
#define MPI_DOUBLE 1
#define MPI_SUM 0
#define MPI_IN_PLACE ((void *)1)
#define MPI_STATUS_IGNORE ((MPI_Status *)0)
#define MPI_THREAD_MULTIPLE 3

static inline int t44_without_mpi(void)
{
    fprintf(stderr, "mpi-and-shmem: built without MPI\n");
    exit(2);
}

static inline int MPI_Init(int *argc, char ***argv) { (void)argc; (void)argv; return t44_without_mpi(); }
static inline int MPI_Init_thread(int *argc, char ***argv, int required, int *provided)
{
    (void)argc; (void)argv; (void)required; (void)provided;
    return t44_without_mpi();
}
static inline int MPI_Finalize(void) { return t44_without_mpi(); }
static inline int MPI_Comm_rank(MPI_Comm comm, int *rank) { (void)comm; (void)rank; return t44_without_mpi(); }
static inline int MPI_Comm_size(MPI_Comm comm, int *size) { (void)comm; (void)size; return t44_without_mpi(); }
static inline int MPI_Allreduce(const void *send, void *receive, int count, MPI_Datatype type,
                                MPI_Op operation, MPI_Comm comm)
{
    (void)send; (void)receive; (void)count; (void)type; (void)operation; (void)comm;
    return t44_without_mpi();
}
static inline int MPI_Barrier(MPI_Comm comm) { (void)comm; return t44_without_mpi(); }
static inline int MPI_Sendrecv(const void *send, int send_count, MPI_Datatype send_type, int destination,
                               int send_tag, void *receive, int receive_count, MPI_Datatype receive_type,
                               int source, int receive_tag, MPI_Comm comm, MPI_Status *status)
{
    (void)send; (void)send_count; (void)send_type; (void)destination; (void)send_tag; (void)receive;
    (void)receive_count; (void)receive_type; (void)source; (void)receive_tag; (void)comm; (void)status;
    return t44_without_mpi();
}
static inline int MPI_Win_create(void *base, long size, int unit, MPI_Info info, MPI_Comm comm, MPI_Win *window)
{
    (void)base; (void)size; (void)unit; (void)info; (void)comm; (void)window;
    return t44_without_mpi();
}
static inline int MPI_Win_fence(int assertion, MPI_Win window) { (void)assertion; (void)window; return t44_without_mpi(); }
static inline int MPI_Win_free(MPI_Win *window) { (void)window; return t44_without_mpi(); }
static inline int MPI_Put(const void *origin, int count, MPI_Datatype type, int target, long displacement,
                          int target_count, MPI_Datatype target_type, MPI_Win window)
{
    (void)origin; (void)count; (void)type; (void)target; (void)displacement; (void)target_count;
    (void)target_type; (void)window;
    return t44_without_mpi();
}
static inline int MPI_Get(void *origin, int count, MPI_Datatype type, int target, long displacement,
                          int target_count, MPI_Datatype target_type, MPI_Win window)
{
    (void)origin; (void)count; (void)type; (void)target; (void)displacement; (void)target_count;
    (void)target_type; (void)window;
    return t44_without_mpi();
}
static inline int MPI_Abort(MPI_Comm comm, int code) { (void)comm; (void)code; return t44_without_mpi(); }
#endif
