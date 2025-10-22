#ifndef MYHTTP_WORKQ_H
#define MYHTTP_WORKQ_H

#include <pthread.h>
#include <stdbool.h>
#include <sys/socket.h>     // struct sockaddr_storage
#include <sys/types.h>      // socklen_t
#include <stddef.h>         // size_t

/* A unit of work: a connected client socket and its peer info. */
struct mh_job {
    int client_fd;
    struct sockaddr_storage peer;
    socklen_t peerlen;
};

/* Bounded MPMC ring-buffer work queue. */
struct mh_workq {
    struct mh_job *ring;
    size_t cap;      /* capacity */
    size_t head;     /* pop index */
    size_t tail;     /* push index */
    size_t count;    /* current items */

    pthread_mutex_t mtx;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;

    bool closed;     /* when true: enqueue/dequeue return -1 */
};

/* Initialize with capacity 'cap' (allocates ring). Returns 0 on success. */
int  workq_init(struct mh_workq *q, size_t cap);

/* Close the queue: wake waiting threads; further enqueues/dequeues fail with -1. */
void workq_close(struct mh_workq *q);

/* Destroy the queue and free resources (must be closed or unused). */
void workq_destroy(struct mh_workq *q);

/* Blocking enqueue; returns 0 on success, -1 if closed. */
int  workq_enqueue(struct mh_workq *q, struct mh_job j);

/* Blocking dequeue; returns 0 on success, -1 if closed and empty. */
int  workq_dequeue(struct mh_workq *q, struct mh_job *out);

#endif /* MYHTTPD_WORKQ_H */

