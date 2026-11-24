// os_unfair_lock on Linux: a plain pthread mutex.
#pragma once
#include <pthread.h>
typedef pthread_mutex_t os_unfair_lock;
#define OS_UNFAIR_LOCK_INIT PTHREAD_MUTEX_INITIALIZER
static inline void os_unfair_lock_lock(os_unfair_lock *l) { pthread_mutex_lock(l); }
static inline void os_unfair_lock_unlock(os_unfair_lock *l) { pthread_mutex_unlock(l); }
