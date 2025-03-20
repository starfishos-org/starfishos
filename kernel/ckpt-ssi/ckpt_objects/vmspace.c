#include "../ckpt_objects.h"
#include "../ckpt_object_pool.h"
#include "../ckpt_page.h"

extern int set_write_in_pgtbl(struct vmspace *vmspace, vaddr_t va, size_t len,
                              bool flag);
extern int set_pte_write_flag(pte_t *entry, bool flag);
extern int is_pte_dirty(pte_t *entry);
extern int pgtbl_deep_copy(vaddr_t *src_pgtbl, vaddr_t *dst_pgtbl);
extern void *create_patch_pool();

#ifdef REPORT
extern u64 pool1_time;
extern u64 ckpt_vmr_array_reuse_count;
extern u64 set_write_in_pgtbl_time;
extern u64 vm_time, vmr_ckpt_time;
extern u64 init_ckpt_vm_time;
#endif

static inline void vmr_ckpt(struct vmregion *target_vmr,
                            struct ckpt_vmregion *ckpt_vmr,
                            int flags)
{
    struct object *old_obj;
    struct ckpt_obj_root *obj_root;
    old_obj = container_of(target_vmr->pmo, struct object, opaque);
    obj_root = ckpt_obj_root_get(old_obj, flags);
    BUG_ON(!obj_root);

    ckpt_vmr->start = target_vmr->start;
    ckpt_vmr->size = target_vmr->size;
    ckpt_vmr->pmo_root = obj_root;
    ckpt_vmr->perm = target_vmr->perm;
}

#ifdef DETAIL_REPORT
extern u64 pool1_time;
extern u64 ckpt_vmr_array_reuse_count;
extern u64 set_write_in_pgtbl_time;
extern u64 vm_time, vmr_ckpt_time;
extern u64 init_ckpt_vm_time;
#endif
int vmspace_ckpt(struct vmspace *target_vmspace,
                 struct ckpt_vmspace *ckpt_vmspace, int flags)
{
#ifdef REPORT
    u64 start0, start1, start2, start3;
    start0 = plat_get_mono_time();
#endif
    struct vmregion *target_vmr;
    struct pte_patch_pool *pool, *next_pool;
    struct ckpt_vmregion *ckpt_vmr_array = NULL;

    target_vmspace->flags |= VM_FLAG_PRESERVE;

    int vmr_count = 0;
    for_each_in_list (target_vmr,
                      struct vmregion,
                      list_node,
                      &(target_vmspace->vmr_list)) {
        vmr_count++;
    }

    if (vmr_count == ckpt_vmspace->vmr_count) {
        /* reuse ckpt vmregion array */
        ckpt_vmr_array = ckpt_vmspace->ckpt_vmrs;
#ifdef DETAIL_REPORT
        ckpt_vmr_array_reuse_count++;
#endif
    } else {
        if (ckpt_vmspace->ckpt_vmrs)
            kfree(ckpt_vmspace->ckpt_vmrs);
        ckpt_vmr_array =
                kmalloc(sizeof(struct ckpt_vmregion) * vmr_count, __SHARED__);
    }

    ckpt_vmspace->user_current_mmap_addr =
            target_vmspace->user_current_mmap_addr;
    ckpt_vmspace->pcid = target_vmspace->pcid;
    ckpt_vmspace->vmr_count = vmr_count;
    ckpt_vmspace->ckpt_vmrs = ckpt_vmr_array;

    int idx = 0;
#ifdef REPORT
    start1 = plat_get_mono_time();
#endif
    for_each_in_list (target_vmr,
                      struct vmregion,
                      list_node,
                      &(target_vmspace->vmr_list)) {
#ifndef OMIT_PF
        if ((target_vmr->perm & VMR_WRITE) != 0
            && !is_external_sync_pmo(target_vmr->pmo) /* do not persist
                                                         ext sync pmo
                                                         each ckpt */
            && !(target_vmspace->pte_patch_pool)) {
            /* set page to unwritable */
            set_write_in_pgtbl(
                    target_vmspace, target_vmr->start, target_vmr->size, false);
        }
#endif
        /* ckpt vmr */
        vmr_ckpt(target_vmr, &ckpt_vmr_array[idx], flags);
        if (unlikely(target_vmr == target_vmspace->heap_vmr)) {
            ckpt_vmspace->heap_vmr_idx = idx;
        }
        idx++;
    }

#ifdef REPORT
    start2 = plat_get_mono_time();
#endif
    if (target_vmspace->pte_patch_pool) {
        /* Use the pool */
        pool = target_vmspace->pte_patch_pool;
        /* Traverse the pte_patch pool */
        for (;;) {
            /*
             * A page(pte) might be freed after it is added to the
             * pte_patch_pool. For this kind of page, set an invalid
             * pte entry entry is harmless, but we should not track
             * access of an unallocated page and migrate it later.
             */
            for (int i = 0; i < pool->count; i++) {
                struct pte_patch_pool_entry entry = pool->array[i];
#ifdef HYBRID_MEM
                /* Now page in pte pool might not be really
                 * accessed. A page might be marked as active
                 * and pre memcpy by
                 * `process_sub_active_list`(ckpt/hot_page_tracker).
                 * We should check here to see if each page is
                 * accessed.
                 */
                if (page_check_flag(entry.page, PG_allocated)
                    && !in_active_list(entry.page->track_info)) {
                    set_pte_write_flag(entry.pte, 0);
                }
#else
                if (page_check_flag(entry.page, PG_allocated)) {
                    set_pte_write_flag(entry.pte, 0);
                }
#endif
            }
            if (pool->next) {
                next_pool = pool->next;
                free_pages(pool);
                pool = next_pool;
            } else {
                /* Traverse pte pool finish; reinit pool */
                pool->count = 0;
                pool->next = 0;
                target_vmspace->pte_patch_pool = (void *)pool;
                break;
            }
        }
    } else {
        /* No pte_patch_pool for current vmspace; init */
        target_vmspace->pte_patch_pool = create_patch_pool();
    }
#ifdef REPORT
    start3 = plat_get_mono_time();
#endif
#ifdef REPORT
    vmr_ckpt_time += start2 - start1;
    pool1_time += start3 - start2;
    init_ckpt_vm_time += start1 - start0;
#endif

    /* check pgtbl */
#ifdef CHECK_PCTBL
    extern bool check_vmspace_unwritable(struct vmspace * vmspace);
    BUG_ON(!check_vmspace_unwritable(target_vmspace));
#endif
    /* now we don't save pgtable */
#ifdef COPY_PGTBL
    pgtbl_deep_copy(target_vmspace->pgtbl, ckpt_vmspace->pgtbl);
#endif
    return 0;
}

static struct vmregion *vmr_restore(struct ckpt_vmregion *ckpt_vmr,
                                    struct kvs *obj_map)
{
    struct vmregion *target_vmr;
    struct object *pmo_obj;

    pmo_obj = restore_obj_get(ckpt_vmr->pmo_root);
    if (!pmo_obj) {
        BUG_ON(1);
    }
    target_vmr = alloc_vmregion();
    if (!target_vmr) {
        kwarn("%s fails\n", __func__);
        goto out_fail;
    }

    target_vmr->start = ckpt_vmr->start;
    target_vmr->size = ckpt_vmr->size;
    target_vmr->pmo = (struct pmobject *)pmo_obj->opaque;
    target_vmr->perm = ckpt_vmr->perm;
#ifdef RMAP_ENABLED
    pmo_add_reverse_node(target_vmr->pmo, target_vmr);
#endif
    return target_vmr;

out_fail:
    return NULL;
}

int vmspace_restore(struct object *vm_obj, struct ckpt_object *ckpt_vm_obj,
                    struct kvs *obj_map, int flags)
{
    int r;
    struct vmspace *target_vmspace = (struct vmspace *)vm_obj->opaque;
    struct ckpt_vmspace *ckpt_vmspace =
            (struct ckpt_vmspace *)ckpt_vm_obj->opaque;
    struct ckpt_vmregion *ckpt_vmr;
    struct vmregion *vmr;
    int vmr_count = ckpt_vmspace->vmr_count;
    int heap_idx = ckpt_vmspace->heap_vmr_idx;

    target_vmspace->pcid = ckpt_vmspace->pcid;
    r = vmspace_init(target_vmspace);
    if (r) {
        BUG_ON(1);
    }

    for (int i = 0; i < vmr_count; i++) {
        ckpt_vmr = &ckpt_vmspace->ckpt_vmrs[i];
        vmr = vmr_restore(ckpt_vmr, obj_map);
        if (!vmr) {
            BUG_ON(1);
        }
        add_vmr_to_vmspace(target_vmspace, vmr);
        if (unlikely(i == heap_idx)) {
            target_vmspace->heap_vmr = vmr;
        }
        // printk("vmspace:%lx, vmr:%lx\n",target_vmspace, vmr);
    }

    target_vmspace->user_current_mmap_addr =
            ckpt_vmspace->user_current_mmap_addr;
    target_vmspace->pcid = ckpt_vmspace->pcid;
    target_vmspace->flags |= VM_FLAG_PRESERVE;
    target_vmspace->pte_patch_pool = create_patch_pool();
#ifdef CHECK_PCTBL
    extern bool check_vmspace_unwritable(struct vmspace * vmspace);
    BUG_ON(!check_vmspace_unwritable(target_vmspace));
#endif
#if COPY_PGTBL
    r = pgtbl_deep_copy(ckpt_vmspace->pgtbl, target_vmspace->pgtbl);
    if (r)
        BUG_ON(1);
#endif
    return r;
}

int ckpt_vmspace_copy(struct ckpt_object *src_obj, struct ckpt_object *dst_obj,
                      struct kvs *obj_map)
{
    struct ckpt_vmspace *src_vmspace, *dst_vmspace;
    int i, r;

    src_vmspace = (struct ckpt_vmspace *)src_obj->opaque;
    dst_vmspace = (struct ckpt_vmspace *)dst_obj->opaque;

    /* Copy basic fields */
    dst_vmspace->pcid = src_vmspace->pcid;
    dst_vmspace->user_current_mmap_addr = src_vmspace->user_current_mmap_addr;
    dst_vmspace->vmr_count = src_vmspace->vmr_count;
    dst_vmspace->heap_vmr_idx = src_vmspace->heap_vmr_idx;

    /* Allocate memory for ckpt_vmrs */
    dst_vmspace->ckpt_vmrs = kmalloc(
            src_vmspace->vmr_count * sizeof(struct ckpt_vmregion), __SHARED__);
    if (!dst_vmspace->ckpt_vmrs) {
        return -ENOMEM;
    }

    /* Copy each vmregion */
    for (i = 0; i < src_vmspace->vmr_count; i++) {
        memcpy(&dst_vmspace->ckpt_vmrs[i],
               &src_vmspace->ckpt_vmrs[i],
               sizeof(struct ckpt_vmregion));

        /* Update the pmo_root using the object map */
        if (src_vmspace->ckpt_vmrs[i].pmo_root) {
            dst_vmspace->ckpt_vmrs[i].pmo_root = get_copied_obj_root(
                    src_vmspace->ckpt_vmrs[i].pmo_root, obj_map);
            if (!dst_vmspace->ckpt_vmrs[i].pmo_root) {
                r = -ENOMEM;
                goto out_free_vmrs;
            }
        }
    }

    return 0;
out_free_vmrs:
    kfree(dst_vmspace->ckpt_vmrs);
    return r;
}
