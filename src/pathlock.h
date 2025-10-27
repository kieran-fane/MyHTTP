#include <pthread.h>

/* Manager object for managing path reading and writing*/
struct path_lock {
    pthread_rwlock_t rw;
    unsigned refcnt;
    struct path_lock *next; // simple chained hash
    char path[];            // NUL-terminated key
};

// Gives lock to reader
int  plock_acquire_rd(const char *abs_path);

// Gives lock to writer
int  plock_acquire_wr(const char *abs_path);

// Release lock to writer and or reader
void plock_release(const char *abs_path);

// INIT
void plock_global_init(void);

// Mem Safe delete
void plock_global_destroy(void);

