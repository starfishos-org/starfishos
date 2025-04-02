#include <arch/mm/page_table.h>
#include <mm/kmalloc.h>
#include <dsm/dsm-mmconfig.h>
#include <mm/vmspace.h>

#include "../ckpt_objects.h"

#define LAZY_PGTBL_RESTORE

int page_table_restore(struct vmspace *vmspace)
{
#ifdef LAZY_PGTBL_RESTORE
    vmspace->flags |= VM_FLAG_PRESERVE;
    vmspace->pgtbl = get_pages(0, __MT_PGTABLE__);
#else
    for (int i = 0; i < vmspace->vmr_list.count; i++) {
        struct vmregion *vmr = vmspace->vmr_list.objs[i];
        map_page_in_pgtbl(vmspace->pgtbl, vmr->start, vmr->pmo->start, vmr->prop, NULL);
    }
#endif
    return 0;
}
