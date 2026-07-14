#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>
#include <errno.h>
#include <debug_lock.h>
#include <syscall_arch.h>
#include <futex.h>
#include <chcore/defs.h>
#include <chcore/bug.h>
#include <chcore/syscall.h>

/*
 * ATTENTION: printf uses futex, so disable futex in __lockfile before using
 *	      printf in futex.
 */
#define chcore_futex_debug 0
#define printf_futex(fmt, ...) do	\
{					\
	if (chcore_futex_debug)		\
		printf("%s:%d " fmt, __FILE__, __LINE__, ##__VA_ARGS__); \
} while (0)

/*
 * TODO: 1. Store waiting events in hash table.
 *       2. Fine-grained lock (e.g., per uaddr).
 *       3. A more proper replacement of sys_notify may be needed.
 *          Because sys_notify will wakeup following sys_wait,
 *          which is a little bit unsuitable for futex.
 */

/*
 * XXX: Requeue from one addr to another addr.
 *	Still use the old notifc_cap.
 */
struct requeued_futex {
	int notifc_cap;
	struct requeued_futex *next;
};

struct futex_entry {
	int notific_cap;
	int *uaddr;
	/* waiter_count does not include requeued ones */
	int waiter_count;
	struct notifc_cache_entry *notifc_cache_entry;

	struct requeued_futex *requeued_futex_list;
};
#define MAX_FUTEX_ENTRIES 256
struct futex_entry futex_entries[MAX_FUTEX_ENTRIES] = { 0 };

/* XXX: Reuse notification created but never free */
struct notifc_cache_entry {
	int notifc_cap;
	struct notifc_cache_entry *next;
};
struct notifc_cache_entry *notifc_cache = NULL;

/*
 * XXX: Every futex op will lock to protect notification cache
 *      and futex_entries.
 */
int volatile futex_lock = 0;

#define FUTEX_CMD_MASK          ~(FUTEX_PRIVATE | FUTEX_CLOCK_REALTIME)

static bool futex_has_waiter(struct futex_entry *entry)
{
	return entry->waiter_count > 0 || entry->requeued_futex_list != NULL;
}

/* Find notifc_cap from cache, or create a new one */
static struct notifc_cache_entry *futex_alloc_notifc()
{
	struct notifc_cache_entry *cur_notifc_cache;
	int notifc_cap;

	if (notifc_cache != NULL) {
		cur_notifc_cache = notifc_cache;
		notifc_cache = notifc_cache->next;
	} else {
		notifc_cap = chcore_syscall0(CHCORE_SYS_create_notifc);
		if (notifc_cap < 0)
			BUG("create notifc failed");
		cur_notifc_cache = malloc(sizeof(*cur_notifc_cache));
		if (!cur_notifc_cache)
			BUG("alloc notifc cache failed");
		cur_notifc_cache->notifc_cap = notifc_cap;
		cur_notifc_cache->next = NULL;
	}

	return cur_notifc_cache;
}

int chcore_futex_wait(int *uaddr, int futex_op, int val,
		      struct timespec *timeout)
{
	int notifc_cap = -1, empty_idx = -1, idx = -1;
	int i, ret;
	struct notifc_cache_entry *cur_notifc_cache;

	chcore_spin_lock(&futex_lock);

	/* check if uaddr contains expected value */
	if (*uaddr != val) {
		chcore_spin_unlock(&futex_lock);
		return -EAGAIN;
	}

	/*
	 * Find if already waited on the same uaddr. If not found,
	 * find a empty entry to put current wait request.
	 */
	for (i = 0; i < MAX_FUTEX_ENTRIES; i++) {
		if (futex_has_waiter(&futex_entries[i])
		    && futex_entries[i].uaddr == uaddr) {
			idx = i;
			break;
		} else if (!futex_has_waiter(&futex_entries[i])) {
			empty_idx = i;
		}
	}

	if (empty_idx < 0 && idx < 0)
		BUG("futex entries overflow");

	if (idx >= 0) {
		/*
		 * 1. waiter_count > 0: already waited on the same uaddr
		 * 2. waiter_count = 0: have requeued waiters
		 */
		notifc_cap = futex_entries[idx].notific_cap;
		futex_entries[idx].waiter_count++;
	} else {
		 /* First one to wait on the uaddr */
		cur_notifc_cache = futex_alloc_notifc();
		BUG_ON(!cur_notifc_cache);
		notifc_cap = cur_notifc_cache->notifc_cap;

		idx = empty_idx;
		futex_entries[idx] = (struct futex_entry) {
			.notific_cap = notifc_cap,
			.uaddr = uaddr,
			.waiter_count = 1,
			.notifc_cache_entry = cur_notifc_cache,
			.requeued_futex_list = NULL
		};
	}
	printf_futex("futex wait:%d addr:%p notifc:%d\n", idx, uaddr, notifc_cap);
	chcore_spin_unlock(&futex_lock);

	/* Try to wait */
	ret = chcore_syscall3(CHCORE_SYS_wait, notifc_cap, true, (long)timeout);

	assert((ret == 0) || (ret == -ETIMEDOUT));

	/* After wake up */
	chcore_spin_lock(&futex_lock);
	futex_entries[idx].waiter_count--;
	/*
	 * If this is the last waiter: not close the notification
	 * but put it in cache in case of following wait.
	 */
	if (!futex_has_waiter(&futex_entries[idx])) {
		cur_notifc_cache = futex_entries[idx]
			           .notifc_cache_entry;
		cur_notifc_cache->next = notifc_cache;
		notifc_cache = cur_notifc_cache;
	}
	chcore_spin_unlock(&futex_lock);

	return 0;
}
/*
 * FIXME: The wake up sequence for requeued request is wrong.
 *	  Requeued waiters are always awoken after waiters which wait on the
 *	  address at beginning. Although this does not affect the
 *        correctness of futex wake.
 */
int chcore_futex_wake(int *uaddr, int futex_op, int val)
{
	int notifc_cap, send_count = 0, idx = -1;
	int i, ret;
	struct requeued_futex *requeued;

	chcore_spin_lock(&futex_lock);
	printf_futex("futex wake uaddr:%p val:%d\n", uaddr, val);
	/* Find waiters with the same uaddr */
	for (i = 0; i < MAX_FUTEX_ENTRIES; i++) {
		if (futex_has_waiter(&futex_entries[i])
		    && futex_entries[i].uaddr == uaddr) {
			idx = i;
			break;
		}
	}

	if (idx == -1) {
		printf_futex("futex wake not found\n");
		send_count = 0;
		goto out_unlock;
	}

	printf_futex("futex wake:%d\n", idx);
	/* 1. send to waiters which wait at beginning */
	notifc_cap = futex_entries[idx].notific_cap;
	send_count = futex_entries[idx].waiter_count;
	send_count = send_count < val ? send_count : val;
	/*
	 * TODO: provide a parameter for notify syscall to notify multiple
	 *	 waiters within one syscall.
	 */
	for (i = 0; i < send_count; i++) {
		printf_futex("\twake orgin cap:%d\n", notifc_cap);
		//ret = chcore_syscall1(CHCORE_SYS_notify, notifc_cap);
		ret = usys_notify(notifc_cap);
		BUG_ON(ret != 0);
	}

	/* 2. send to requeued waiters */
	requeued = futex_entries[idx].requeued_futex_list;
	printf_futex("futex wake send_count:%d queue:%p\n", send_count, requeued);
	while (send_count < val && requeued) {
		printf_futex("\twake queue cap:%d\n", requeued->notifc_cap);
		//ret = chcore_syscall1(CHCORE_SYS_notify, requeued->notifc_cap);
		ret = usys_notify(requeued->notifc_cap);
		BUG_ON(ret != 0);
		futex_entries[idx].requeued_futex_list = requeued->next;
		free(requeued);
		requeued = futex_entries[idx].requeued_futex_list;
	}

out_unlock:
	chcore_spin_unlock(&futex_lock);

	return send_count;
}

/*
 * TODO: Currently only support requeue one waiter.
 */
int chcore_futex_requeue(int *uaddr, int *uaddr2, int nr_wake, int nr_requeue)
{
	BUG_ON(nr_wake != 0);
	BUG_ON(nr_requeue != 1);
	int empty_idx = -1, send_count = 0, idx = -1, idx2 = -1;
	int i, ret;
	struct requeued_futex *requeue_iter, *requeue;
	struct notifc_cache_entry *cur_notifc_cache;

	if (uaddr == uaddr2)
		return -EINVAL;

	chcore_spin_lock(&futex_lock);
	for (i = 0; i < MAX_FUTEX_ENTRIES; i++) {
		if (futex_has_waiter(&futex_entries[i])
		    && futex_entries[i].uaddr == uaddr) {
			idx = i;
		} else if (futex_has_waiter(&futex_entries[i])
			   && futex_entries[i].uaddr == uaddr2) {
			idx2 = i;
		} else if (!futex_has_waiter(&futex_entries[i])) {
			empty_idx = i;
		}

		if (idx != -1 && idx2 != -1)
			break;
	}

	if (idx == -1) {
		printf_futex("futex requeue not found\n");
		chcore_spin_unlock(&futex_lock);
		return -EINVAL;
	}

	if (idx2 == -1 && empty_idx == -1)
		BUG("futex entries overflow");

	/* requeue target does not contain any waiter */
	if (idx2 == -1) {
		idx2 = empty_idx;
		cur_notifc_cache = futex_alloc_notifc();
		BUG_ON(!cur_notifc_cache);

		futex_entries[idx2] = (struct futex_entry) {
			.notific_cap = cur_notifc_cache->notifc_cap,
			.uaddr = uaddr2,
			.waiter_count = 0,
			.notifc_cache_entry = cur_notifc_cache,
			.requeued_futex_list = NULL
		};
	}

	if (idx2 == -1 && empty_idx == -1)
		BUG("futex entries overflow");

	if (futex_entries[idx].waiter_count != 0) {
		/* requeue a waiter that wait on the address at beginning */
		requeue = malloc(sizeof(*requeue));
		if (!requeue)
			BUG("out of memory");
		requeue->next = NULL;
		requeue->notifc_cap = futex_entries[idx].notific_cap;
	} else {
		/* requeue a waiter that is requeued previously */
		requeue = futex_entries[idx].requeued_futex_list;
		futex_entries[idx].requeued_futex_list
			= futex_entries[idx].requeued_futex_list->next;
		requeue->next = NULL;
	}

	/* add to the tail of requeued list */
	requeue_iter = futex_entries[idx2].requeued_futex_list;
	printf_futex("futex requeue old:%d addr:%p cap:%d new:%d addr:%p\n",
		     idx, uaddr, requeue->notifc_cap,
		     idx2, uaddr2);
	if (!requeue_iter) {
		futex_entries[idx2].requeued_futex_list = requeue;
	} else {
		while (requeue_iter->next)
			requeue_iter = requeue_iter->next;
		requeue_iter->next = requeue;
	}

	chcore_spin_unlock(&futex_lock);
	return 0;
}

int chcore_futex(int *uaddr, int futex_op, int val,
		 struct timespec *timeout, int *uaddr2, int val3)
{
	int cmd;
	int val2;
	if ((futex_op & FUTEX_PRIVATE) == 0) {
		BUG("futex sharing not supported\n");
		return -ENOSYS;
	}
	cmd = futex_op & FUTEX_CMD_MASK;

	switch (cmd) {
	case FUTEX_WAIT:
		return chcore_futex_wait(uaddr, futex_op, val, timeout);
	case FUTEX_WAKE:
		return chcore_futex_wake(uaddr, futex_op, val);
	case FUTEX_REQUEUE:
		val2 = (long)timeout;
		return chcore_futex_requeue(uaddr, uaddr2, val, val2);
	default:
		BUG("futex op not implemented\n");
	}
	return -ENOSYS;
}
