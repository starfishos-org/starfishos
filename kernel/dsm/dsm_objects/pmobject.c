#include <mm/vmspace.h>
#include <mm/page.h>

#include "../dsm_tiering.h"

static void dsm_copy_page(vaddr_t dst, vaddr_t src)
{
    // __asm__ __volatile__("clflush %0" : : "m"(src));
    pagecpy((void *)dst, (void *)src);
}

static int __dsm_copy_radix(struct radix_node *src,
                            struct radix_node *dst,
                            int node_level,
                            mem_t page_mem_type)
{
    int err;
    int i;
    struct radix_node *new;
    
    if (node_level == RADIX_LEVELS - 1) {
        for (i = 0; i < RADIX_NODE_SIZE; i++) {
            if (!src->values[i] && dst->values[i]) {
                void *pa = dst->values[i];
                dst->values[i] = NULL;
                kfree((void *)phys_to_virt(pa));
                continue;
            }
            if (dst->values[i]) {
                dsm_copy_page(phys_to_virt(dst->values[i]),
                        phys_to_virt(src->values[i]));
            } else {
                void *newpage = get_pages(0, page_mem_type);
                BUG_ON(!newpage);
                dsm_copy_page((vaddr_t)newpage, phys_to_virt(src->values[i]));
                dst->values[i] = (void *)virt_to_phys(newpage);
            }
        }
        return 0;
    }

    for (i = 0; i < RADIX_NODE_SIZE; i++) {
        if (src->children[i]) {
            new = kzalloc(sizeof(struct radix_node), page_mem_type);
            if (IS_ERR(new)) {
                return -ENOMEM;
            }
            dst->children[i] = new;
            err = __dsm_copy_radix(src->children[i], dst->children[i], 
                                   node_level + 1, page_mem_type);
            if (err) {
                return err;
            }
        }
    }

    return 0;
}

int dsm_copy_radix(struct radix *src, struct radix *dst, mem_t page_mem_type)
{
    int r;
    struct radix_node *new;
    BUG_ON(!(src && dst));
    r = 0;

    /* don't need to lock dst */
    lock(&src->radix_lock);

    if (!src->root) {
        goto out;
    }

    if (!dst->root) {
        new = kzalloc(sizeof(struct radix_node), page_mem_type);
        if (IS_ERR(new)) {
            r = -ENOMEM;
        }
        dst->root = new;
    }

    r = __dsm_copy_radix(src->root, dst->root, 0, page_mem_type);
out:
    unlock(&src->radix_lock);
    return r;
}

int dsm_copy_pmo(struct object *src_obj, struct object *dst_obj)
{
    struct pmobject *src_pmo = (struct pmobject *)src_obj->opaque;
    struct pmobject *dst_pmo = (struct pmobject *)dst_obj->opaque;
    mem_t page_mem_type = is_private_object(src_obj) ? __MT_SHARED__ : __MT_PRIVATE__;

    /* Copy basic fields */
    dst_pmo->size = src_pmo->size;
    dst_pmo->start = src_pmo->start;
    dst_pmo->type = src_pmo->type;
    dst_pmo->mm_type = page_mem_type;

    if (src_pmo->type == PMO_FILE) {
        dst_pmo->private = src_pmo->private;
        // TODO: more handle for PMO_FILE
    }

    /* Copy radix tree */
    if (is_continuous_pmo(src_pmo)) {
        void *new_va = kmalloc(dst_pmo->size, page_mem_type);
        if (!new_va) {
            return -ENOMEM;
        }

        dst_pmo->start = (paddr_t)virt_to_phys(new_va);

        for (int i = 0; i < DIV_ROUND_UP(src_pmo->size, PAGE_SIZE); i++) {
            u64 dst_pa = dst_pmo->start + i * PAGE_SIZE;
            u64 src_pa = src_pmo->start + i * PAGE_SIZE;
            dsm_copy_page(phys_to_virt(dst_pa), phys_to_virt(src_pa));
        }
    } else if (is_radix_pmo(src_pmo)) {
        /* init radix tree if not exists*/
        if (!dst_pmo->radix) {
            dst_pmo->radix = new_radix(page_mem_type);
            if (!dst_pmo->radix) {
                return -ENOMEM;
            }
            init_radix(dst_pmo->radix);
        }

        /* Deep copy the radix tree */
        int r = dsm_copy_radix(src_pmo->radix, dst_pmo->radix, page_mem_type);
        if (r) {
            kfree(dst_pmo->radix);
            return r;
        }
    } else {
        // Do nothing
    }

    return 0;
}

int dsm_stw_copy_pmo(struct object *src_obj, struct object *dst_obj)
{
    return 0;
}
