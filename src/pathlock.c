#ifndef PLOCK_NBUCKETS
#define PLOCK_NBUCKETS 256u
#endif

struct bucket {
	struct path_lock *head;
};

static struct {
	int initialized;
	pthread_mutex_t map_mtx;
	struct bucket buckets[PLOCK_NBUCKETS];
} g_plm = {
	.initialized = 0,
	.map_mtx = PTHREAD_MUTEX_INITIALIZER
};


// ---- Internal helpers ----

static uint32_t fnv1a_32(const char *s) {
	const uint8_t *p = (const uint8_t *)s;
	uint32_t h = 2166136261u;
	while (*p) {
		h ^= (uint32_t)*p++;
		h *= 16777619u;
	}
	return h;
}

static inline unsigned bucket_idx_from_path(const char *abs_path) {
	return (unsigned)(fnv1a_32(abs_path) % PLOCK_NBUCKETS);
}
// Lookup existing; returns pointer or NULL (does not modify refcnt).
static struct path_lock *pl_lookup_unlocked(const char *abs_path, unsigned idx) {
	struct path_lock *pl = g_plm.buckets[idx].head;
	while (pl) {
		if (strcmp(pl->path, abs_path) == 0) return pl;
		pl = pl->next;
	}
	return NULL;
}

// Create a new entry with refcnt=1; on success returns pointer (inserted).
// Requires g_plm.map_mtx to be held by the caller.
static struct path_lock *pl_create_locked(const char *abs_path) {
	size_t n = strlen(abs_path) + 1;
	struct path_lock *pl = (struct path_lock *)malloc(sizeof(*pl) + n);
	if (!pl) { errno = ENOMEM; return NULL; }

	int rc = pthread_rwlock_init(&pl->rw, NULL);
	if (rc != 0) {
		free(pl);
		errno = rc; // map pthread error to errno-ish int
		return NULL;
	}

	pl->refcnt = 1;
	pl->next = NULL;
	memcpy(pl->path, abs_path, n);
	// Insert at bucket head
	unsigned idx = bucket_idx_from_path(abs_path);
	pl->next = g_plm.buckets[idx].head;
	g_plm.buckets[idx].head = pl;
	return pl;
}

// Remove and destroy an entry. Requires g_plm.map_mtx to be held and refcnt==0.
static void pl_remove_and_destroy_locked(struct path_lock *pl, unsigned idx) {
    	struct path_lock **pp = &g_plm.buckets[idx].head;
    	while (*pp) {
        	if (*pp == pl) {
            		*pp = pl->next;
            		pthread_rwlock_destroy(&pl->rw);
            		free(pl);
            		return;
        	}
        	pp = &(*pp)->next;
   	}
   	// If we reach here, something is inconsistent; ignore in release builds.
}


//----- API ----------

void plock_global_init(void) {
	if (__atomic_load_n(&g_plm.initialized, __ATOMIC_ACQUIRE)) return;
	pthread_mutex_lock(&g_plm.map_mtx);
	if (!g_plm.initialized) {
		for (unsigned i = 0; i < PLOCK_NBUCKETS; ++i) {
			g_plm.buckets[i].head = NULL;
		}
		g_plm.initialized = 1;
	}
	pthread_mutex_unlock(&g_plm.map_mtx);
}

void plock_global_destroy(void) {
	pthread_mutex_lock(&g_plm.map_mtx);
	for (unsigned i = 0; i < PLOCK_NBUCKETS; ++i) {
		struct path_lock *pl = g_plm.buckets[i].head;
		g_plm.buckets[i].head = NULL;
		while (pl) {
			struct path_lock *next = pl->next;
			// Best-effort: caller should ensure no one holds these locks anymore.
			pthread_rwlock_destroy(&pl->rw);
			free(pl);
			pl = next;
		}
	}
	g_plm.initialized = 0;
	pthread_mutex_unlock(&g_plm.map_mtx);
}

int plock_acquire_rd(const char *abs_path) {
	if (!abs_path || !*abs_path) { errno = EINVAL; return -1; }
	if (!g_plm.initialized) plock_global_init();

	const unsigned idx = bucket_idx_from_path(abs_path);

	// Fast path: find or create under map mutex, bump refcnt.
	pthread_mutex_lock(&g_plm.map_mtx);
	struct path_lock *pl = pl_lookup_unlocked(abs_path, idx);
	if (pl) {
		++pl->refcnt;
		pthread_mutex_unlock(&g_plm.map_mtx);
	} else {
		pl = pl_create_locked(abs_path);
		pthread_mutex_unlock(&g_plm.map_mtx);
		if (!pl) return -1; // errno already set
	}

	// Acquire shared lock outside map mutex to avoid blocking the table.
	int rc = pthread_rwlock_rdlock(&pl->rw);
	if (rc != 0) {
		// Roll back refcnt if lock failed.
		pthread_mutex_lock(&g_plm.map_mtx);
		// Find again (might have moved only if bugs).
		struct path_lock *pl2 = pl_lookup_unlocked(abs_path, idx);
		if (pl2) {
			if (--pl2->refcnt == 0) {
				pl_remove_and_destroy_locked(pl2, idx);
			}
        	}
		pthread_mutex_unlock(&g_plm.map_mtx);
		errno = rc;
		return -1;
	}
	return 0;
}

int plock_acquire_wr(const char *abs_path) {
	if (!abs_path || !*abs_path) { errno = EINVAL; return -1; }
	if (!g_plm.initialized) plock_global_init();

	const unsigned idx = bucket_idx_from_path(abs_path);

	pthread_mutex_lock(&g_plm.map_mtx);
	struct path_lock *pl = pl_lookup_unlocked(abs_path, idx);
	if (pl) {
		++pl->refcnt;
		pthread_mutex_unlock(&g_plm.map_mtx);
	} else {
		pl = pl_create_locked(abs_path);
		pthread_mutex_unlock(&g_plm.map_mtx);
		if (!pl) return -1;
	}

	int rc = pthread_rwlock_wrlock(&pl->rw);
	if (rc != 0) {
		pthread_mutex_lock(&g_plm.map_mtx);
		struct path_lock *pl2 = pl_lookup_unlocked(abs_path, idx);
		if (pl2) {
			if (--pl2->refcnt == 0) {
				pl_remove_and_destroy_locked(pl2, idx);
			}
		}
		pthread_mutex_unlock(&g_plm.map_mtx);
		errno = rc;
		return -1;
	}
	return 0;
}

void plock_release(const char *abs_path) {
    	if (!abs_path || !*abs_path) return;
    	if (!g_plm.initialized) return;

    	const unsigned idx = bucket_idx_from_path(abs_path);

    	// Locate under map mutex (but do not mutate yet)
    	pthread_mutex_lock(&g_plm.map_mtx);
    	struct path_lock *pl = pl_lookup_unlocked(abs_path, idx);
    	pthread_mutex_unlock(&g_plm.map_mtx);
    	if (!pl) {
       		// Nothing to release (maybe double-release or shutdown); ignore
        	return;
	}

    	// If this thread didn't hold it, behavior is undefined by pthreads.
	(void)pthread_rwlock_unlock(&pl->rw);

	// Now dec refcnt and possibly destroy the entry.
	pthread_mutex_lock(&g_plm.map_mtx);
	struct path_lock *pl2 = pl_lookup_unlocked(abs_path, idx);
	if (pl2) {
		if (--pl2->refcnt == 0) {
			pl_remove_and_destroy_locked(pl2, idx);
		}
	}
	pthread_mutex_unlock(&g_plm.map_mtx);
}


