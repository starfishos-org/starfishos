#pragma once

#include <common/types.h>

/* Simple/Compact ticket/rwlock impl */

struct lock {
	volatile u64 slock;
};

struct rwlock {
	volatile u32 lock;
};

int lock_init(struct lock *lock);
void lock(struct lock *lock);
/* returns 0 on success, -1 otherwise */
int try_lock(struct lock *lock);
void unlock(struct lock *lock);
int is_locked(struct lock *lock);

/* Global locks */
extern struct lock big_kernel_lock;

int rwlock_init(struct rwlock *rwlock);
void read_lock(struct rwlock *rwlock);
void write_lock(struct rwlock *rwlock);
/* read/write_try_lock return 0 on success, -1 otherwise */
int read_try_lock(struct rwlock *rwlock);
int write_try_lock(struct rwlock *rwlock);
void read_unlock(struct rwlock *rwlock);
void write_unlock(struct rwlock *rwlock);
void rw_downgrade(struct rwlock *rwlock);
