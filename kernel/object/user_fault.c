#ifdef CHCORE_ENABLE_FMAP

#include <arch/mmu.h>
#include <ipc/notification.h>
#include <irq/irq.h>
#include <common/errno.h>
#include <common/radix.h>
#include <common/lock.h>
#include <sched/sched.h>
#include <mm/kmalloc.h>
#include <lib/printk.h>
#include <mm/vmspace.h>
#include <mm/kmalloc.h>
#include <mm/mm.h>
#include <mm/buddy.h>
#include <object/object.h>
#include <object/user_fault.h>
#include <lib/ring_buffer.h>
#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
#include <ckpt/ckpt_data.h>
#include <ckpt/ckpt.h>
#endif

extern int trans_uva_to_kva(u64 user_va, u64 *kernel_va);

struct lock fmap_fault_pool_list_lock;
struct list_head fmap_fault_pool_list;

typedef u64 pte_t;
void add_pte_patch_to_pool(struct vmspace *vmspace, pte_t *pte,
                           struct page *page);
int map_page_in_pgtbl(void *pgtbl, vaddr_t va, paddr_t pa, vmr_prop_t flags,
                      pte_t **out_pte);
int track_access(struct page *page);

static int inited = 0;

static void user_fault_init(void)
{
        int inited = 0;

        if (__sync_bool_compare_and_swap(&inited, 0, 1)) {
                lock_init(&fmap_fault_pool_list_lock);
                init_list_head(&fmap_fault_pool_list);
        }
}

void reset_user_fault_init(void)
{
        inited = 0;
}

static struct fmap_fault_pool *get_current_fault_pool(void)
{
        int badge;
        struct fmap_fault_pool *pool_iter;

        badge = current_cap_group->badge;
        for_each_in_list (pool_iter,
                          struct fmap_fault_pool,
                          node,
                          &fmap_fault_pool_list) {
                if (pool_iter->cap_group_badge == badge) {
                        return pool_iter;
                }
        }

        return NULL;
}

static struct fault_pending_thread *get_current_pending_thread(u64 client_badge,
                                                               vaddr_t fault_va)
{
        struct fault_pending_thread *pt;
        struct fmap_fault_pool *pool;

        pool = get_current_fault_pool();
        if (!pool)
                return NULL;

        for_each_in_list (
                pt, struct fault_pending_thread, node, &pool->pending_threads) {
                if (pt->fault_badge == client_badge
                    && pt->fault_va == fault_va) {
                        return pt;
                }
        }
        return NULL;
}

/* syscall */
int sys_user_fault_register(int notific_cap, vaddr_t msg_buffer)
{
        int ret;
        struct notification *notific;
        struct ring_buffer *msg_buffer_kva;
        /* *msg_buffer_kva points to the virtual address of a ring buffer
         * struct, so no need to initialize */
        u64 badge;
        struct fmap_fault_pool *pool_iter;

        user_fault_init();

        badge = current_cap_group->badge;

        /* Validate arguments */
        notific = obj_get(current_cap_group, notific_cap, TYPE_NOTIFICATION);
        if (notific == NULL) {
                return -EINVAL;
        }

        ret = trans_uva_to_kva(msg_buffer, (vaddr_t *)&msg_buffer_kva);
        if (ret != 0) {
                return -EINVAL;
        }

        /*
         * FIXME:
         * We can only allow one process (fs) to register the mmap fault handler
         * thread for once.
         */
        lock(&fmap_fault_pool_list_lock);
        if (get_current_fault_pool() != NULL) {
                /* pool already exists */
                unlock(&fmap_fault_pool_list_lock);
                return -EINVAL;
        }

        /* Create a fmap_fault_pool and add to list */
        pool_iter = (struct fmap_fault_pool *)kmalloc(sizeof(*pool_iter), __DEFAULT__);
        if (!pool_iter) {
                unlock(&fmap_fault_pool_list_lock);
                return -ENOMEM;
        }

        pool_iter->cap_group_badge = badge;
        pool_iter->notific = notific;
        pool_iter->msg_buffer_kva = msg_buffer_kva;
        lock_init(&pool_iter->lock);
        init_list_head(&pool_iter->pending_threads);

        list_append(&pool_iter->node, &fmap_fault_pool_list);
        unlock(&fmap_fault_pool_list_lock);

        return 0;
}

int get_pa_from_handler_vmr(struct vmregion *vmr, vaddr_t va, paddr_t *pa)
{
        u64 offset;
        u64 index;
        struct pmobject *pmo = vmr->pmo;
        switch (pmo->type) {
        case PMO_ANONYM:
        case PMO_DATA: {
                offset = ROUND_DOWN(va, PAGE_SIZE) - vmr->start;
                index = offset / PAGE_SIZE;
                *pa = get_page_from_pmo(pmo, index);
                break;
        }
        default: {
                printk("pmo->type = %d\n", pmo->type);
                return -EINVAL;
        }
        }
        return 0;
}

int sys_user_fault_map(u64 client_badge, vaddr_t fault_va, vaddr_t remap_va,
                       bool copy)
{
        struct fmap_fault_pool *current_pool;
        struct fault_pending_thread *pending_thread;
        struct thread *thread_to_wake;
        struct vmspace *handler_vmspace;
        struct vmspace *fault_vmspace;
        paddr_t pa, new_pa;
        void *new_page;
        int perm;
        int ret;
        u64 *pte;
        struct vmregion *vmr = NULL, *handler_vmr = NULL;
        struct pmobject *pmo = NULL;
        u64 offset, index = 0;

        current_pool = get_current_fault_pool();

        /* Find corresponding pending thread */
        lock(&current_pool->lock);
        pending_thread = get_current_pending_thread(client_badge, fault_va);
        if (!pending_thread) {
                unlock(&current_pool->lock);
                return -EINVAL;
        }
        list_del(&pending_thread->node);
        unlock(&current_pool->lock);

        thread_to_wake = pending_thread->thread;
        kfree(pending_thread);

        /* Get handler space va, which page will be mapped in fault va */
        if (remap_va) {
                handler_vmspace = obj_get(
                        current_cap_group, VMSPACE_OBJ_ID, TYPE_VMSPACE);
                lock(&handler_vmspace->pgtbl_lock);
                extern int query_in_pgtbl(void *, vaddr_t, paddr_t *, void **);
                ret = query_in_pgtbl(
                        handler_vmspace->pgtbl, remap_va, &pa, NULL);
                if (ret) {
                        read_lock(&handler_vmspace->vmspace_lock);
                        handler_vmr =
                                find_vmr_for_va(handler_vmspace, remap_va);
                        ret = get_pa_from_handler_vmr(
                                handler_vmr, remap_va, &pa);
                        read_unlock(&handler_vmspace->vmspace_lock);
                        if (ret) {
                                /* remap_va is not mapped in handler_vmspace */
                                unlock(&handler_vmspace->pgtbl_lock);
                                obj_put(handler_vmspace);
                                return -EINVAL;
                        }
                }
                unlock(&handler_vmspace->pgtbl_lock);
                obj_put(handler_vmspace);
        }

        fault_vmspace = obj_get(
                thread_to_wake->cap_group, VMSPACE_OBJ_ID, TYPE_VMSPACE);
        write_lock(&fault_vmspace->vmspace_lock);
        /* Decide whether copy the physical page or share */
        if (!copy) {
                if (!remap_va)
                        return -EINVAL;
                new_pa = pa;
        } else {
                // new_page = get_dram_pages(0);
                new_page = get_pages(0, __DEFAULT__);
                if (remap_va)
                        memcpy(new_page, (void *)phys_to_virt(pa), PAGE_SIZE);
                else
                        memset(new_page, 0, PAGE_SIZE);
                new_pa = (paddr_t)virt_to_phys(new_page);

                vmr = find_vmr_for_va(fault_vmspace, fault_va);
                BUG_ON(vmr == NULL);
                pmo = vmr->pmo;
                offset = fault_va - vmr->start;
                index = offset / PAGE_SIZE;
                commit_page_to_pmo(pmo, index, new_pa);

                if (offset + PAGE_SIZE > pmo->size) {
                        pmo->size = offset + PAGE_SIZE;
                }
        }
        /* Fill fault pa with target page's pa */
        lock(&fault_vmspace->pgtbl_lock);
        /* FIXME: we never consider overlapped fmap here */
        perm = VMR_READ | VMR_WRITE | VMR_EXEC;
        ret = map_page_in_pgtbl(
                fault_vmspace->pgtbl, fault_va, new_pa, perm, &pte);
        BUG_ON(ret);
        unlock(&fault_vmspace->pgtbl_lock);

#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
        /* Add pte patch */
        if (fault_vmspace->flags & VM_FLAG_PRESERVE) {
                /*  vmspace lock is not get */
                struct page *page = virt_to_page((void *)phys_to_virt(new_pa));
//     int ckpt_ret =
#ifndef OMIT_BENCHMARK
                #ifdef CHCORE_SSI_SLS
                ckpt_dsm_page(pmo, (void *)phys_to_virt(new_pa), index);
                #else
                ckpt_nvm_page(pmo, (void *)phys_to_virt(new_pa), index);
                #endif
#endif
                add_pte_patch_to_pool(fault_vmspace, pte, page);
                //     if(ckpt_ret) {
                //         track_access(page);
                //     }
        }
#endif
        write_unlock(&fault_vmspace->vmspace_lock);
        obj_put(fault_vmspace);

        /* Pending thread should come back to scheduler */
        thread_to_wake->thread_ctx->state = TS_INTER;
        BUG_ON(sched_enqueue(thread_to_wake));

        return 0;
}

/* Handling a user page fault */
void handle_user_fault(struct pmobject *pmo, vaddr_t fault_va)
{
        struct fmap_fault_pool *fault_pool;
        struct fault_pending_thread *pending_thread;
        int ret;

        fault_pool = (struct fmap_fault_pool *)pmo->private;
        kdebug("pmo file fault: badge=%lx, va=%lx\n",
               fault_pool->cap_group_badge,
               fault_va);

        /*
         * Fault thread should pending until user handling finished.
         * Record (fault_badge, fault_va) -> thread here.
         */
        pending_thread =
                (struct fault_pending_thread *)kmalloc(sizeof(*pending_thread), __DEFAULT__);
        if (!pending_thread) {
                /* TODO: handle no memory */
                BUG_ON(1);
        }

        pending_thread->fault_badge = current_cap_group->badge;
        pending_thread->fault_va = fault_va;
        pending_thread->thread = current_thread;

        /* The fault_pool lock also protect producer ptr racing */
        lock(&fault_pool->lock);

        if (if_buffer_full(fault_pool->msg_buffer_kva)) {
                /* TODO: user ring buffer is full */
                BUG_ON(1);
        } else {
                /* successfully fetch slot from server space */
                struct user_fault_msg tmp;
                tmp.fault_badge = current_cap_group->badge;
                tmp.fault_va = fault_va;
                set_one_msg(fault_pool->msg_buffer_kva, &tmp);
        }
        list_append(&pending_thread->node, &fault_pool->pending_threads);

        /* Notify the fault handler when buffer is updated */
        ret = signal_notific(fault_pool->notific);
        BUG_ON(ret != 0);

        /*
         * Give up the control flow here.
         * The thread will wake up when map finished.
         */
        current_thread->thread_ctx->state = TS_WAITING;

        sched();
        /*
         * To avoid sys_user_fault_map get pending thread too early,
         *      or modify thread->state early than here.
         * Release lock here.
         */
        unlock(&fault_pool->lock);
        eret_to_thread(switch_context());
}
#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
int fmap_fault_pool_create_ckpt(struct list_head *ckpt_fmap_fault_pool_list)
{
        struct ckpt_fmap_fault_pool *ckpt_pool_iter, *ckpt_pool_iter_tmp;
        struct ckpt_fault_pending_thread *ckpt_pt, *ckpt_pt_tmp;
        if (ckpt_fmap_fault_pool_list->next)
                for_each_in_list_safe (ckpt_pool_iter,
                                       ckpt_pool_iter_tmp,
                                       node,
                                       ckpt_fmap_fault_pool_list) {
                        for_each_in_list_safe (
                                ckpt_pt,
                                ckpt_pt_tmp,
                                node,
                                &ckpt_pool_iter->ckpt_fault_pending_thread_list) {
                                kfree(ckpt_pt);
                        }
                        kfree(ckpt_pool_iter->msg_buffer_kva);
                        kfree(ckpt_pool_iter);
                }
        init_list_head(ckpt_fmap_fault_pool_list);
        struct fmap_fault_pool *pool_iter;
        extern struct ckpt_obj_root *ckpt_obj_root_get(struct object * obj,
                                                       int alloc);
        for_each_in_list (pool_iter,
                          struct fmap_fault_pool,
                          node,
                          &fmap_fault_pool_list) {
                struct fault_pending_thread *pt;
                #ifdef CHCORE_SLS
                ckpt_pool_iter = kmalloc(sizeof(struct ckpt_fmap_fault_pool), __DEFAULT__);
                #else
                ckpt_pool_iter = kmalloc(sizeof(struct ckpt_fmap_fault_pool), __SHARED__);
                #endif
                init_list_head(&ckpt_pool_iter->ckpt_fault_pending_thread_list);

                for_each_in_list (pt,
                                  struct fault_pending_thread,
                                  node,
                                  &pool_iter->pending_threads) {
                        #ifdef CHCORE_SLS
                        ckpt_pt = kmalloc(
                                sizeof(struct ckpt_fault_pending_thread), __DEFAULT__);
                        #else
                        ckpt_pt = kmalloc(
                                sizeof(struct ckpt_fault_pending_thread), __SHARED__);
                        #endif

                        ckpt_pt->fault_badge = pt->fault_badge;
                        ckpt_pt->fault_va = pt->fault_va;
                        ckpt_pt->ckpt_thread_obj_root = ckpt_obj_root_get(
                                container_of(pt->thread, struct object, opaque),
                                false);
                        list_add(
                                &ckpt_pt->node,
                                &ckpt_pool_iter->ckpt_fault_pending_thread_list);
                }

                ckpt_pool_iter->msg_buffer_kva =
                        ckpt_ring_buffer(pool_iter->msg_buffer_kva);

                ckpt_pool_iter->cap_group_badge = pool_iter->cap_group_badge;
                ckpt_pool_iter->ckpt_notifc_obj_root = ckpt_obj_root_get(
                        container_of(pool_iter->notific, struct object, opaque),
                        false);
                ckpt_pool_iter->fmap_fault_pool = (vaddr_t)pool_iter;
                list_add(&ckpt_pool_iter->node, ckpt_fmap_fault_pool_list);
        }
        return 0;
}

int fmap_fault_pool_restore(struct list_head *ckpt_fmap_fault_pool_list,
                            struct kvs *obj_map)
{
        /* TODO: recycle old fmap fault pool*/
        user_fault_init();

        struct ckpt_fmap_fault_pool *ckpt_pool_iter;
        extern struct object *restore_obj_get(struct ckpt_obj_root
                                              * ckpt_obj_root);
        for_each_in_list (ckpt_pool_iter,
                          struct ckpt_fmap_fault_pool,
                          node,
                          ckpt_fmap_fault_pool_list) {
                struct fmap_fault_pool *pool_iter;
                struct ckpt_fault_pending_thread *ckpt_pt;

                pool_iter = (struct fmap_fault_pool *)
                                    ckpt_pool_iter->fmap_fault_pool;

                /* TODO: recycle old pending_threads */
                init_list_head(&pool_iter->pending_threads);
                lock_init(&pool_iter->lock);
                for_each_in_list (
                        ckpt_pt,
                        struct ckpt_fault_pending_thread,
                        node,
                        &ckpt_pool_iter->ckpt_fault_pending_thread_list) {
                        struct fault_pending_thread *pt;
                        #ifdef CHCORE_SLS
                        pt = kmalloc(sizeof(struct fault_pending_thread), __DEFAULT__);
                        #else
                        pt = kmalloc(sizeof(struct fault_pending_thread), __SHARED__);
                        #endif

                        pt->fault_badge = ckpt_pt->fault_badge;
                        pt->fault_va = ckpt_pt->fault_va;
                        pt->thread = (struct thread *)restore_obj_get(
                                             ckpt_pt->ckpt_thread_obj_root)
                                             ->opaque;
                        list_add(&pt->node, &pool_iter->pending_threads);
                }

                struct ring_buffer *tmp_buffer =
                        restore_ring_buffer(ckpt_pool_iter->msg_buffer_kva);
                BUG_ON(tmp_buffer != pool_iter->msg_buffer_kva);
                pool_iter->cap_group_badge = ckpt_pool_iter->cap_group_badge;
                pool_iter->notific =
                        (struct notification *)restore_obj_get(
                                ckpt_pool_iter->ckpt_notifc_obj_root)
                                ->opaque;
                BUG_ON(!pool_iter->notific);
                list_add(&pool_iter->node, &fmap_fault_pool_list);
                kdebug("list add badge=%lx,pool->pending_thread=%lx,pool=%lx\n",
                       pool_iter->cap_group_badge,
                       &pool_iter->pending_threads,
                       pool_iter);
        }

        return 0;
}
#endif /* CHCORE_SLS */
#endif
