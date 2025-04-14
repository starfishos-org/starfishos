#include <mm/vmspace.h>
#include <arch/mm/page_table.h>

#include "../dsm_tiering.h"

extern int pgtbl_deep_copy(vaddr_t *src_pgtbl, vaddr_t *dst_pgtbl, mem_t mem_type);

int dsm_copy_vmregion(struct vmregion *src_vmr, struct vmregion *dst_vmr, mem_t mem_type)
{
    dst_vmr->start = src_vmr->start;
    dst_vmr->size = src_vmr->size;
    dst_vmr->perm = src_vmr->perm;

    struct object *src_pmo_obj, *dst_pmo_obj;
    src_pmo_obj = obj2object(src_vmr->pmo);
    int ret = dsm_demote_object(obj2object(src_vmr->pmo));
    if (ret) {
        return ret;
    }
    dst_pmo_obj = dsm_get_object_by_mem_type(src_pmo_obj, mem_type, false);
    if (!dst_pmo_obj) {
        return -ENOMEM;
    }
    dst_vmr->pmo = (struct pmobject *)object2obj(dst_pmo_obj);
    return 0;
}

int dsm_copy_vmspace(struct object *src_obj, struct object *dst_obj)
{
    struct vmspace *src_vmspace = (struct vmspace *)src_obj->opaque;
    struct vmspace *dst_vmspace = (struct vmspace *)dst_obj->opaque;
    int is_demote = is_private_object(src_obj);
    mem_t mem_type = is_demote ? __MT_SHARED__ : __MT_PRIVATE__;

    /* Copy basic fields */
    dst_vmspace->user_current_mmap_addr = src_vmspace->user_current_mmap_addr;
    dst_vmspace->pcid = src_vmspace->pcid;
    dst_vmspace->flags = src_vmspace->flags;

    /* Initialize vmregion list */
    init_list_head(&dst_vmspace->vmr_list);

    /* Copy vmregions */
    struct vmregion *src_vmr;
    for_each_in_list(src_vmr, struct vmregion, list_node, &src_vmspace->vmr_list) {
        struct vmregion *dst_vmr = alloc_vmregion(mem_type);
        if (!dst_vmr) {
            return -ENOMEM;
        }

        /* Copy vmregion */
        dsm_copy_vmregion(src_vmr, dst_vmr, mem_type);

        /* Add to list */
        add_vmr_to_vmspace(dst_vmspace, dst_vmr);

        /* Update heap_vmr if needed */
        if (src_vmr == src_vmspace->heap_vmr) {
            dst_vmspace->heap_vmr = dst_vmr;
        }
    }

    /* Init page table */
    dst_vmspace->pgtbl = get_pages(0, mem_type);
    memset(dst_vmspace->pgtbl, 0, PAGE_SIZE);
    dst_vmspace->flags |= VM_FLAG_PRESERVE;

    return 0;
}

int dsm_stw_copy_vmspace(struct object *src_obj, struct object *dst_obj)
{
    return 0;
}
