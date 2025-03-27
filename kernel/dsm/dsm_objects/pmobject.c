#include <mm/vmspace.h>

#include "../dsm_tiering.h"

int dsm_copy_pmo(struct object *src_obj, struct object *dst_obj)
{
    struct pmobject *src_pmo = (struct pmobject *)src_obj->opaque;
    struct pmobject *dst_pmo = (struct pmobject *)dst_obj->opaque;

    /* Copy basic fields */
    dst_pmo->size = src_pmo->size;
    dst_pmo->start = src_pmo->start;
    dst_pmo->type = src_pmo->type;

    /* Copy radix tree */
    if (src_pmo->radix) {
        /* init radix tree */
        dst_pmo->radix = kzalloc(sizeof(struct radix), __MT_SHARED__);
        if (!dst_pmo->radix) {
            return -ENOMEM;
        }
        init_radix(dst_pmo->radix);

        /* Deep copy the radix tree */
        // int r = ckpt_radix_deep_copy(src_pmo->radix, dst_pmo->radix, 0);
        // if (r) {
        //     kfree(dst_pmo->radix);
        //     return r;
        // }
    } else {
        dst_pmo->radix = NULL;
    }

    return 0;
}
