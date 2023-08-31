#pragma once

#include <pthread.h>

struct lock {
	pthread_mutex_t mutex;
};

static inline int lock_init(struct lock *lock)
{
	pthread_mutex_init(&lock->mutex, NULL);
	return 0;
}

static inline void lock(struct lock *lock)
{
	pthread_mutex_lock(&lock->mutex);
}

static inline int try_lock(struct lock *lock)
{
	return pthread_mutex_trylock(&lock->mutex) == 0;
}

static inline void unlock(struct lock *lock)
{
	pthread_mutex_unlock(&lock->mutex);
}

static inline int is_locked(struct lock *lock)
{
	int r = pthread_mutex_trylock(&lock->mutex);
	if (r == 0)
		pthread_mutex_unlock(&lock->mutex);
	return r != 0;
}

/* FIXME: dummy read write lock just for temporary use */
struct rwlock {
	struct lock lock;
};

static inline int rwlock_init(struct rwlock *rwlock)
{
	return lock_init(&rwlock->lock);
}

static inline void read_lock(struct rwlock *rwlock)
{
	lock(&rwlock->lock);
}

static inline void write_lock(struct rwlock *rwlock)
{
	lock(&rwlock->lock);
}

static inline int read_try_lock(struct rwlock *rwlock)
{
	return try_lock(&rwlock->lock);
}

static inline int write_try_lock(struct rwlock *rwlock)
{
	return try_lock(&rwlock->lock);
}

static inline void read_unlock(struct rwlock *rwlock)
{
	unlock(&rwlock->lock);
}

static inline void write_unlock(struct rwlock *rwlock)
{
	unlock(&rwlock->lock);
}

