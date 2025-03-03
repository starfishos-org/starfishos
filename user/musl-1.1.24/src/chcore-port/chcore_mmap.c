#include <stdio.h>
#include <chcore/defs.h>
#include <chcore/memory.h>
#include <chcore/syscall.h>
#include <pthread.h>
#include <chcore/container/list.h>

#include "chcore_mman.h"

/*
 * When a thread is created, it mmaps a chunk of memory for the thread stack and
 * TLS, which is reclaimed when the thread exits (by calling unmapself).
 * Unmapself is a piece of assembly code that jumps to munmap to reclaim
 * associated memory.
 *
 * Linux implements mmap/munmap in kernel mode, so unmapself.s reclaims the
 * associated memory directly through system calls. Becasue the user/kernel
 * stacks of a thread are different. After the user stack is reclaimed in the
 * kernel, the control flow can still return unmapself and make other system
 * calls without using the thread stack.
 *
 * Since chcore mmap is implemented in user mode and relevant mmap information
 * is recorded in user mode. When a thread exits, relevant data structures need
 * to be recycled. Therefore, once the thread stack is released and relevant
 * data structures are recycled through C code, it cannot return to unmapself
 * again. And subsequent thread_exit system calls cannot be executed. Therefore,
 * the user version of unmapself needs to maintain a common stack that can be
 * used as a temporary thread stack when the thread exits.
 */

static bool __initial_common_stack_success = false;

struct htable va2pmo;
/* For sequential access. */
static struct list_head pmo_node_head;
static pthread_spinlock_t va2pmo_lock;

/* For unmapself. */
static int common_stack_pmo_cap;
static vaddr_t common_stack_addr;
pthread_spinlock_t common_stack_lock;

/* For initial once. */
pthread_once_t init_mmap_once = PTHREAD_ONCE_INIT;
pthread_once_t init_common_stack_once = PTHREAD_ONCE_INIT;

static void initial_mmap(void)
{
        pthread_spin_init(&va2pmo_lock, 0);
        init_htable(&va2pmo, HASH_TABLE_SIZE);
        init_list_head(&pmo_node_head);
}

static void initial_common_stack(void)
{
        u64 prot;
        vaddr_t stack_bottom_addr;
        int ret = 0;

        pthread_spin_init(&common_stack_lock, 0);

        common_stack_pmo_cap =
                usys_create_pmo(UNMAPSELF_STACK_SIZE, PMO_ANONYM, MALLOC_TYPE_DEFAULT);
        if (common_stack_pmo_cap < 0) {
                printf("Error occur on create unmapself pmo\n");
                ret = common_stack_pmo_cap;
                goto fail_out;
        }

        stack_bottom_addr = (vaddr_t)chcore_alloc_vaddr(UNMAPSELF_STACK_SIZE);
        if (!stack_bottom_addr) {
                printf("Error occur on alloc vaddr\n");
                ret = -ENOMEM;
                goto revoke_pmo_cap;
        }

        /* Prepare the common stack for thread exiting. */
        prot = PROT_READ | PROT_WRITE;
        ret = usys_map_pmo(
                SELF_CAP, common_stack_pmo_cap, stack_bottom_addr, prot);
        if (ret < 0) {
                printf("Error occur on map stack of unmapself\n");
                goto free_addr;
        }
        common_stack_addr = stack_bottom_addr + UNMAPSELF_STACK_SIZE;
        __initial_common_stack_success = true;
        return;

free_addr:
        chcore_free_vaddr(common_stack_addr, UNMAPSELF_STACK_SIZE);
revoke_pmo_cap:
        usys_revoke_cap(common_stack_pmo_cap);
fail_out:
        __initial_common_stack_success = false;
}

static struct pmo_node *new_pmo_node(int cap, vaddr_t va, size_t length)
{
        struct pmo_node *node;

        node = (struct pmo_node *)malloc(sizeof(struct pmo_node));
        node->cap = cap;
        node->va = va;
        node->pmo_size = length;
        init_hlist_node(&node->hash_node);
        return node;
}

static inline void free_pmo_node(struct pmo_node *node)
{
        if (node) {
                free(node);
        }
}

/* Find the first pmo which fits the condition. */
static struct pmo_node *get_next_pmo_node(void *va, int length,
                                          struct pmo_node *start_pmo_node)
{
        struct hlist_head *buckets;
        struct pmo_node *node = NULL;
        struct list_head *start;

        if (!start_pmo_node) {
                buckets = htable_get_bucket(&va2pmo, VA_TO_KEY(va));

                for_each_in_hlist (node, hash_node, buckets) {
                        if (node->va == (vaddr_t)va) {
                                goto out;
                        }
                }
                start = &pmo_node_head;
        } else {
                start = &start_pmo_node->list_node;
        }

        if (start->next == &pmo_node_head) {
                goto fail;
        }

        for_each_in_list (node, struct pmo_node, list_node, start) {
                if ((u64)node->va >= (u64)va
                    && (u64)node->va + node->pmo_size <= (u64)va + length) {
                        goto out;
                }
        }

fail:
        node = NULL;
out:
        return node;
}

/* Insert the node to list in order of virtual addresses */
static void add_node_in_order(struct pmo_node *node)
{
        struct pmo_node *temp;

        if (list_empty(&pmo_node_head)) {
                list_add(&node->list_node, &pmo_node_head);
                return;
        }

        for_each_in_list (temp, struct pmo_node, list_node, &pmo_node_head) {
                if ((u64)temp->va > (u64)node->va) {
                        list_add(&node->list_node, temp->list_node.prev);
                        return;
                }
        }
        list_add(&node->list_node, pmo_node_head.prev);
}
/* TODO: Take the lock before allocating and mapping/free and unmapping
 * address.*/
void *chcore_mmap(void *start, size_t length, int prot, int flags, int fd,
                  off_t off, int pmo_cap)
{
        struct pmo_node *node;
        void *map_addr;
        int ret;

        if (fd != -1) {
                printf("%s: here only supports anonymous mapping with fd -1, but arg fd is %d\n",
                       __func__,
                       fd);
                goto err_exit;
        }

        /* Check @prot */
        if (prot & PROT_CHECK_MASK) {
                printf("%s: here cannot support PROT: %d\n", __func__, prot);
                goto err_exit;
        }

        /* Check @flags */
        if (flags != (MAP_ANONYMOUS | MAP_PRIVATE) && flags != (MAP_ANONYMOUS | MAP_PRIVATE | MAP_CXL)) {
                printf("%s: here only supports anonymous, private and shared mapping\n",
                       __func__);
                goto err_exit;
        }

        /* Round up @length */
        if (length % PAGE_SIZE) {
                length = ROUND_UP(length, PAGE_SIZE);
        }

        pthread_once(&init_mmap_once, initial_mmap);

        /* pmo create */
        if (pmo_cap == 0) {
                if (flags & MAP_CXL) {
                        pmo_cap = usys_create_pmo(length, PMO_ANONYM, MALLOC_TYPE_SHARED);
                } else {
                        pmo_cap = usys_create_pmo(length, PMO_ANONYM, MALLOC_TYPE_DEFAULT);
                }
        }
        
        if (pmo_cap <= 0) {
                printf("Fail: cannot create the new pmo for mmap\n");
                goto err_exit;
        }

        map_addr = (void *)chcore_alloc_vaddr(length);
        if (map_addr == NULL) {
                goto err_free_pmo;
        }

        node = new_pmo_node(pmo_cap, (vaddr_t)map_addr, length);
        if (node == NULL) {
                goto err_free_addr;
        }
        pthread_spin_lock(&va2pmo_lock);
        htable_add(&va2pmo, VA_TO_KEY(map_addr), &node->hash_node);
        add_node_in_order(node);
        pthread_spin_unlock(&va2pmo_lock);

        /* map pmo */
        if (usys_map_pmo(SELF_CAP, pmo_cap, (vaddr_t)map_addr, prot) != 0) {
                goto err_free_node;
        }

        return map_addr;

err_free_node:
        htable_del(&node->hash_node);
        list_del(&node->list_node);
        free_pmo_node(node);
err_free_addr:
        chcore_free_vaddr((u64)map_addr, length);
err_free_pmo:
        usys_revoke_cap(pmo_cap);
err_exit:
        map_addr = (void *)(-1);
        return map_addr;
}

/* TODO: we just support unmap the whole pmo. */
int chcore_munmap(void *start, size_t length)
{
        int pmo_cap;
        int ret = 0;
        size_t pmo_size;
        u64 addr;
        u64 end_addr;
        struct pmo_node *node;
        struct pmo_node *prev_node = NULL;

        /* What if the len is not aligned */
        if ((vaddr_t)start % PAGE_SIZE) {
                printf("%s: start (0x%llu) is not page-aligned\n", __func__, (vaddr_t)start);
                ret = -EINVAL;
                return ret;
        }

        if (length % PAGE_SIZE) {
                length = ROUND_UP(length, PAGE_SIZE);
        }

        addr = (u64)start;
        end_addr = (u64)start + length;
        while (length != 0) {
                pthread_spin_lock(&va2pmo_lock);
                node = get_next_pmo_node((void *)addr, length, prev_node);
                if (node == NULL) {
                        pthread_spin_unlock(&va2pmo_lock);
                        if (prev_node)
                                free_pmo_node(prev_node);
                        return ret;
                }

                pmo_cap = node->cap;
                pmo_size = node->pmo_size;
                addr = node->va;

                hlist_del(&node->hash_node);
                list_del(&node->list_node);
                pthread_spin_unlock(&va2pmo_lock);
                if (prev_node)
                        free_pmo_node(prev_node);

                usys_unmap_pmo(SELF_CAP, pmo_cap, (vaddr_t)addr);
                usys_revoke_cap(pmo_cap);
                chcore_free_vaddr(addr, pmo_size);

                addr += pmo_size;
                length = end_addr - addr;
                prev_node = node;
        }

        free_pmo_node(node);
        return ret;
}

/*
 * Lock the common stack and return the top addr of the stack.
 *
 * If the thread unmap it's thread stack, it can not make any function call. So,
 * we need to switch the thread stack to another stack and call some collection
 * functions (including stack collection). Then, release the common stack lock
 * before traping to the kernel.
 *
 * If the common stack initialization fails, the current thread stack cannot be
 * released, so the system call of thread_exit is called directly, and the
 * thread stack is released when the entire CAPgroup is reclaimed.
 */
vaddr_t chcore_lock_common_stack(void)
{
        pthread_once(&init_common_stack_once, initial_common_stack);
        if (unlikely(!__initial_common_stack_success)) {
                printf("init common stack failed!\n");
                usys_exit(0);
        }

        pthread_spin_lock(&common_stack_lock);
        return common_stack_addr;
}

/*
 * Unmap the thread stack before thread exit.
 * We must go back to unmapself.s and release the lock in assembly code.
 * Otherwise, other threads may use the common stack after the current thread
 * releases the lock, thereby breaking the return address of the current thread
 * on the common stack.
 */
vaddr_t chcore_unmapself(void *start, size_t length)
{
        chcore_munmap(start, length);
        return (vaddr_t)&common_stack_lock;
}
