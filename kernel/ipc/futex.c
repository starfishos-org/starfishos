/*
 * Copyright (c) 2023 Institute of Parallel And Distributed Systems (IPADS), Shanghai Jiao Tong University (SJTU)
 * Licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include <common/errno.h>
#include <ipc/futex.h>
#include <ipc/notification.h>
#include <common/lock.h>
#include <common/util.h>
#include <mm/kmalloc.h>
#include <mm/uaccess.h>
#include <object/object.h>

#define BUCKET_SIZE 16
#define UADDR_TO_KEY(uaddr)   ((u32)((long)uaddr % (1 << 12)))
#define FUTEX_CMD_MASK ~(FUTEX_PRIVATE | FUTEX_CLOCK_REALTIME)

static bool futex_has_waiter(struct futex_entry *entry)
{
        return entry->waiter_count > 0;
}

static void free_futex_entry(struct futex_entry *entry)
{
        htable_del(&entry->hash_node);
        deinit_notific(entry->notific);
        kfree(entry->notific);
        kfree(entry);
}

static struct futex_entry *new_futex_entry(struct futex *futex, int *uaddr, struct hlist_head *buckets, mem_t mem_type)
{
        struct notification *notifc;
        struct futex_entry *new_entry;

        new_entry = kmalloc(sizeof(struct futex_entry), mem_type);
        if (!new_entry)
                goto out_fail;

        notifc = kmalloc(sizeof(*notifc), mem_type);
        if (!notifc) {
                goto out_free_entry;
        }
        init_notific(notifc);

        *new_entry = (struct futex_entry){
                .notific = notifc,
                .uaddr = uaddr,
                .waiter_count = 0};
        hlist_add(&new_entry->hash_node, buckets);

        return new_entry;

out_free_entry:
        kfree(new_entry);
out_fail:
        return NULL;
}

static struct futex_entry* find_entry(struct futex *futex, int *uaddr, struct hlist_head **buckets)
{
        struct futex_entry *found_entry= NULL, *entry = NULL;

        *buckets = htable_get_bucket(&futex->futex_entries, UADDR_TO_KEY(uaddr));
        for_each_in_hlist (entry, hash_node, *buckets) {
                if (futex_has_waiter(entry) && entry->uaddr == uaddr) {
                        found_entry = entry;
                        break;
                }
        }

        return found_entry;
}

void futex_init(struct futex *futex, mem_t mem_type)
{
        lock_init(&futex->futex_lock);
        init_htable(&futex->futex_entries, BUCKET_SIZE);
        futex->mem_type = mem_type;
}

void futex_deinit(struct futex *futex)
{
        struct futex_entry *entry, *temp;
        int i;

        for_each_in_htable_safe (entry, temp, i, hash_node, &futex->futex_entries)
                free_futex_entry(entry);
        htable_free(&futex->futex_entries);
}

int futex_copy(struct futex *src_futex, struct futex *dst_futex, mem_t dst_mem_type)
{
        struct futex_entry *entry, *temp, *new_entry;
        struct hlist_head *buckets;
        int i;

        lock_init(&dst_futex->futex_lock);
        init_htable(&dst_futex->futex_entries, BUCKET_SIZE);
        dst_futex->mem_type = dst_mem_type;

        for_each_in_htable_safe (entry, temp, i, hash_node, &src_futex->futex_entries) {
                // copy old entry to new entry
                buckets = htable_get_bucket(&dst_futex->futex_entries, UADDR_TO_KEY(entry->uaddr));
                new_entry = new_futex_entry(dst_futex, entry->uaddr, buckets, dst_futex->mem_type);
                if (new_entry == NULL) {
                        return -ENOMEM;
                }
                new_entry->waiter_count = entry->waiter_count;
                new_entry->notific = (struct notification *)obj2objpair(entry->notific);
        }

        return 0;
}

static void __check_and_free(struct futex_entry *entry)
{
        if (!futex_has_waiter(entry)) {
                free_futex_entry(entry);
        }
}
static void __inc_waiter_count(struct futex_entry *entry)
{
        entry->waiter_count++;
}
static void __dec_waiter_count(struct futex_entry *entry)
{
        entry->waiter_count--;
        __check_and_free(entry);
}

int sys_futex_wait(int *uaddr, int futex_op, int val, struct timespec *timeout)
{
        struct futex *futex = current_cap_group->futex;
        struct lock *futex_lock = &futex->futex_lock;
        struct futex_entry *found_entry;
        struct hlist_head *buckets = NULL;
        int ret, kval;

        lock(futex_lock);

        /* check if uaddr contains expected value */
        if (check_user_addr_range((vaddr_t)uaddr, sizeof(int)) != 0) {
                ret = -EFAULT;
                goto out_unlock;
        }

        ret = copy_from_user((void *)&kval, (void *)uaddr, sizeof(int));
        if (ret != 0) {
                ret = -EFAULT;
                goto out_unlock;
        }

        if (kval != val) {
                ret = -EAGAIN;
                goto out_unlock;
        }

        /*
         * Find if already waiting on the same uaddr. If not found,
         * create an empty entry to put current wait request.
         */
        found_entry = find_entry(futex, uaddr, &buckets);
        if (found_entry == NULL) {
                found_entry = new_futex_entry(futex, uaddr, buckets, futex->mem_type);
                if (found_entry == NULL) {
                        ret = -ENOMEM;
                        goto out_unlock;
                }
        }
        __inc_waiter_count(found_entry);

        /* Try to wait */
        ret = wait_notific_internal(found_entry->notific, true, NULL, true, false);

        __dec_waiter_count(found_entry);

        /* Still hold futex_lock now due to normal C return to this point. */
out_unlock:
        unlock(futex_lock);
        return ret;
}

int sys_futex_wake(int *uaddr, int futex_op, int val)
{
        struct futex *futex = current_cap_group->futex;
        struct lock *futex_lock = &futex->futex_lock;
        struct futex_entry *found_entry;
        struct hlist_head *buckets = NULL;
        struct notification *notifc;
        int send_count;
        int i, ret;

        lock(futex_lock);

        /* Find waiters with the same uaddr */
        found_entry = find_entry(futex, uaddr, &buckets);

        if (!found_entry) {
                ret = 0;
                goto out_unlock;
        }

        /* send to waiters */
        notifc = found_entry->notific;
        send_count = found_entry->waiter_count;
        send_count = send_count < val ? send_count : val;
        for (i = 0; i < send_count; i++) {
                if (signal_notific(notifc) != 0) {
                        break;
                }
                __dec_waiter_count(found_entry);
        }

        ret = i;

out_unlock:
        unlock(futex_lock);

        return ret;
}

int sys_futex_requeue(int *uaddr, int *uaddr2, int nr_wake, int nr_requeue)
{
        struct futex *futex = current_cap_group->futex;
        struct lock *futex_lock = &futex->futex_lock;
        struct futex_entry *found_entry1= NULL, *found_entry2 = NULL;
        struct hlist_head *buckets = NULL;
        int ret = 0;

        /* Currently only support requeue one waiter. */
        if (nr_wake != 0 || nr_requeue != 1)
                return -ENOSYS;
        if (uaddr == uaddr2)
                return -EINVAL;

        lock(futex_lock);
        found_entry1 = find_entry(futex, uaddr, &buckets);
        found_entry2 = find_entry(futex, uaddr2, &buckets);

        if (found_entry1 == NULL) {
                ret = -EINVAL;
                goto out_unlock;
        }


        /* requeue target does not contain any waiter */
        if (found_entry2 == NULL) {
                found_entry2 = new_futex_entry(futex, uaddr2, buckets, futex->mem_type);
                if (found_entry2 == NULL) {
                        ret = -ENOMEM;
                        goto out_unlock;
                }
        }

        ret = requeue_notific(found_entry1->notific, found_entry2->notific);

        /* update entry */
        if (ret == 0) {
                __dec_waiter_count(found_entry1);
                __inc_waiter_count(found_entry2);
        } else {
                __check_and_free(found_entry2);
        }

out_unlock:
        unlock(futex_lock);
        return ret;
}

int sys_futex(int *uaddr, int futex_op, int val, struct timespec *timeout,
                 int *uaddr2, int val3)
{
        int cmd;
        int val2;
        if ((futex_op & FUTEX_PRIVATE) == 0) {
                return -ENOSYS;
        }
        cmd = futex_op & FUTEX_CMD_MASK;

        switch (cmd) {
        case FUTEX_WAIT:
                return sys_futex_wait(uaddr, futex_op, val, timeout);
        case FUTEX_WAKE:
                return sys_futex_wake(uaddr, futex_op, val);
        case FUTEX_REQUEUE:
                val2 = (long) timeout;
                return sys_futex_requeue(uaddr, uaddr2, val, val2);
        default:
                return -ENOSYS;
        }
}
