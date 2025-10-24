#define _POSIX_C_SOURCE 200809L

#include "workq.h"

#include <stdlib.h>
#include <errno.h>
#include <string.h>

static inline size_t next_index(size_t i, size_t cap) {
    	++i;
    	return (i == cap) ? 0 : i;
}

int workq_init(struct mh_workq *q, size_t cap) {
	if (!q || cap == 0) { errno = EINVAL; return -1; }

	memset(q, 0, sizeof(*q)); // Zero's out where the struct is stored for safety

	q->ring = (struct mh_job *)malloc(sizeof(struct mh_job) * cap);
	if (!q->ring) { errno = ENOMEM; return -1; }

	q->cap = cap;
	q->head = 0;
	q->tail = 0;
	q->count = 0;
	q->closed = false;

	if (pthread_mutex_init(&q->mtx, NULL) != 0) {
		int saved = errno;
		free(q->ring); q->ring = NULL;
		errno = saved ? saved : EINVAL; return -1;
	}
	if (pthread_cond_init(&q->not_empty, NULL) != 0) {
		int saved = errno;
		pthread_mutex_destroy(&q->mtx);
		free(q->ring); q->ring = NULL;
		errno = saved ? saved : EINVAL; return -1;
    	}
	if (pthread_cond_init(&q->not_full, NULL) != 0) {
		int saved = errno;
		pthread_cond_destroy(&q->not_empty);
		pthread_mutex_destroy(&q->mtx);
		free(q->ring); q->ring = NULL;
		errno = saved ? saved : EINVAL;
		return 1;
	}
	return 0;
}

void workq_close(struct mh_workq *q) {
	if (!q) return;
	pthread_mutex_lock(&q->mtx);
	q->closed = true;
	// WAKE UP THE WAITING THREADS
	pthread_cond_broadcast(&q->not_empty);
	pthread_cond_broadcast(&q->not_full);
	pthread_mutex_unlock(&q->mtx);
}

void workq_destroy(struct mh_workq *q) {
	if (!q) return;
	// CALLER TO ENSURE NO WAITERS/USE HERE
	pthread_cond_destroy(&q->not_full);
	pthread_cond_destroy(&q->not_empty);
	pthread_mutex_destroy(&q->mtx);
	free(q->ring); q->ring = NULL;
	q->cap = q->head = q->tail = q->count = 0;
	q->closed = true;
}

int workq_enqueue(struct mh_workq *q, struct mh_job j) {
	if (!q) { errno = EINVAL; return -1; }

	pthread_mutex_lock(&q->mtx);
	while (!q->closed && q->count == q->cap) {
		// WAIT FOR AVAILABLE SPACE AND THAT THE QUEUE ISN'T CLOSED
		pthread_cond_wait(&q->not_full, &q->mtx);
	}
	if (q->closed) {
		// WE HAVE A CLOSED QUEUE
		pthread_mutex_unlock(&q->mtx);
		errno = EINVAL; return -1;
	}
	// PUSH
	q->ring[q->tail] = j;
	q->tail = next_index(q->tail, q->cap);
	q->count++;
	// SIGNAL THAT SOMETHING IS IN THE QUEUE
	pthread_cond_signal(&q->not_empty);
	pthread_mutex_unlock(&q->mtx);
	return 0;
}

int workq_dequeue(struct mh_workq *q, struct mh_job *out) {
	if (!q || !out) { errno = EINVAL; return -1; }
	pthread_mutex_lock(&q->mtx);
	while (!q->closed && q->count == 0) {
		// WAIT UNTIL AN ITEM ARRIVES OR QUEUE CLOSES
		pthread_cond_wait(&q->not_empty, &q->mtx);
	}
	if (q->count == 0 && q->closed) {
		pthread_mutex_unlock(&q->mtx);
		errno = EINVAL; return -1;
	}
	// POP THE JOB
	*out = q->ring[q->head];
	q->head = next_index(q->head, q->cap);
	q->count--;
	// SIGNAL THAT THERE IS A FREE SLOT
	pthread_cond_signal(&q->not_full);
	pthread_mutex_unlock(&q->mtx);
	return 0;
}
