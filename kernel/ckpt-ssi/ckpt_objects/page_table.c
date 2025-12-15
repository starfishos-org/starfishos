#include <arch/mm/page_table.h>
#include <mm/kmalloc.h>
#include <dsm/dsm-mmconfig.h>
#include <mm/vmspace.h>
#ifdef DSM_ENABLED
#include <dsm/dsm-single.h>
#endif

#include "../ckpt_objects.h"

#define LAZY_PGTBL_RESTORE

int page_table_restore(struct vmspace *vmspace)
{
#ifdef LAZY_PGTBL_RESTORE
    vmspace->flags |= VM_FLAG_PRESERVE;
#ifdef MULTI_PAGETABLE_ENABLED
    for (int i = 0; i < CLUSTER_MAX_MACHINE_NUM; i++) {
        vmspace->pgtbl[i] = get_pages(0, __MT_PGTABLE__);
        memset(vmspace->pgtbl[i], 0, PAGE_SIZE);
    }
#else
    vmspace->pgtbl = get_pages(0, __MT_PGTABLE__);
    memset(vmspace->pgtbl, 0, PAGE_SIZE);
#endif
#else
    void *pgtbl;
#ifdef MULTI_PAGETABLE_ENABLED
    pgtbl = get_vmspace_pgtbl(vmspace, CUR_MACHINE_ID);
#else
    pgtbl = vmspace->pgtbl;
#endif
    for (int i = 0; i < vmspace->vmr_list.count; i++) {
        struct vmregion *vmr = vmspace->vmr_list.objs[i];
        map_page_in_pgtbl(pgtbl, vmr->start, vmr->pmo->start, vmr->prop, NULL);
    }
#endif
    return 0;
}
