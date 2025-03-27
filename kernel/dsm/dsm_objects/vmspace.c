#include <mm/vmspace.h>

#include "../dsm_tiering.h"

int dsm_copy_vmspace(struct object *src_obj, struct object *dst_obj)
{
    struct vmspace *src_vmspace = (struct vmspace *)src_obj->opaque;
    struct vmspace *dst_vmspace = (struct vmspace *)dst_obj->opaque;

    /* Copy basic fields */
    dst_vmspace->user_current_mmap_addr = src_vmspace->user_current_mmap_addr;
    dst_vmspace->pcid = src_vmspace->pcid;
    dst_vmspace->flags = src_vmspace->flags;

    /* Initialize vmregion list */
    init_list_head(&dst_vmspace->vmr_list);

    /* Copy vmregions */
    struct vmregion *src_vmr;
    for_each_in_list(src_vmr, struct vmregion, list_node, &src_vmspace->vmr_list) {
        struct vmregion *dst_vmr = alloc_vmregion();
        if (!dst_vmr) {
            return -ENOMEM;
        }

        /* Copy vmregion fields */
        dst_vmr->start = src_vmr->start;
        dst_vmr->size = src_vmr->size;
        dst_vmr->perm = src_vmr->perm;
        dst_vmr->pmo = src_vmr->pmo; /* PMO will be handled separately */

        /* Add to list */
        list_append(&dst_vmr->list_node, &dst_vmspace->vmr_list);

        /* Update heap_vmr if needed */
        if (src_vmr == src_vmspace->heap_vmr) {
            dst_vmspace->heap_vmr = dst_vmr;
        }
    }

    return 0;
}
