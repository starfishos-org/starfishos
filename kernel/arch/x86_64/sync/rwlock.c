#include <common/types.h>
#include <common/errno.h>
#include <common/macro.h>
#include <common/lock.h>
#include <common/kprint.h>
#include <arch/sync.h>

/* Simple RWLock */

int rwlock_init(struct rwlock *rwlock)
{
    if (rwlock == 0)
        return -EINVAL;
    rwlock->lock = 0;
    return 0;
}

/* WARN: when there are more than 0x7FFFFFFF readers exist, this function
 * will not function correctly */
void read_lock(struct rwlock *rwlock)
{
    while (atomic_fetch_add_32((s32 *)&rwlock->lock, 1) & 0x80000000) {
        /* Use atomic load to ensure we see the latest value from CXL shared memory */
        while (atomic_load_32((s32 *)&rwlock->lock) & 0x80000000) {
            CPU_PAUSE();
            /* Handle IPI while waiting to avoid deadlock */
            extern void handle_ipi(void);
            handle_ipi();
        }
    }
    COMPILER_BARRIER();
}

int read_try_lock(struct rwlock *rwlock)
{
    s32 old;

    old = atomic_fetch_add_32((s32 *)&rwlock->lock, 1);
    COMPILER_BARRIER();
    return (old & 0x80000000) ? -1 : 0;
}

void read_unlock(struct rwlock *rwlock)
{
    COMPILER_BARRIER();
    atomic_fetch_add_32(&rwlock->lock, -1);
}

void write_lock(struct rwlock *rwlock)
{
    while (compare_and_swap_32((s32 *)&rwlock->lock, 0, 0x80000000) != 0) {
        CPU_PAUSE();
        /* Handle IPI while waiting to avoid deadlock */
        extern void handle_ipi(void);
        handle_ipi();
    }
    COMPILER_BARRIER();
}

int write_try_lock(struct rwlock *rwlock)
{
    int ret = 0;

    if (compare_and_swap_32((s32 *)&rwlock->lock, 0, 0x80000000) != 0)
        ret = -1;
    COMPILER_BARRIER();
    return ret;
}

void write_unlock(struct rwlock *rwlock)
{
    COMPILER_BARRIER();
    /* Use atomic store to ensure visibility across machines in CXL shared memory */
    atomic_store_32((s32 *)&rwlock->lock, 0);
    /* Memory barrier to ensure the unlock is visible to other machines */
    mb();
}
