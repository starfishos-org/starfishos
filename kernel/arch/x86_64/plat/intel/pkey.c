/*
 * This file is used for configuring Intel MPK memory domains.
 */

#include <mm/mm.h>
#include <mm/vmspace.h>
#include <mm/kmalloc.h>
#include <common/kprint.h>
#include <common/macro.h>
#include <common/types.h>
#include <common/errno.h>
#include <arch/mm/page_table.h>

#ifdef MULTI_PAGETABLE_ENABLED
#include <dsm/dsm-single.h>
#endif

/*
 * Pkey only takes effect on the last level page table page (pte)
 * which points to some physical page.
 *
 */
static void pte_set_domain(pte_t *entry, int domid)
{
    /* the pkey in pte_2M or pte_1G are same with pte_4K */
    entry->pte_4K.pkey = domid;
}

#ifdef CHCORE

extern int get_next_ptp(ptp_t *, u32, vaddr_t, ptp_t **, pte_t **, bool);

// #define PCID_RMASK (0xfffUL)
// #define remove_pcid(x) (void *)(((u64)pgtbl) & (~PCID_RMASK))
// #define get_pcid(x) (((u64)pgtbl) & (PCID_RMASK))

/*
 * Function pkey_protect will modify page table entries.
 * However, it does not flush tlb.
 * The caller should flush tlb by using the most efficient way
 * (e.g., single flush or batch flush).
 */
int pkey_protect(struct vmspace *vmspace, vaddr_t va, size_t len, int domain_id)
{
    void *pgtbl;

    s64 total_page_cnt;
    ptp_t *l0_ptp, *l1_ptp, *l2_ptp, *l3_ptp;
    pte_t *pte;
    pte_t *entry;
    int ret;
    int pte_index; /* the index of pte in the last level page table */
    int i;

    /* va should be page aligned: which is done in vmspace_map_range */
    BUG_ON(va % PAGE_SIZE);

    /* root page table page must exist */
#ifdef MULTI_PAGETABLE_ENABLED
    pgtbl = get_vmspace_pgtbl(vmspace, CUR_MACHINE_ID);
#else
    pgtbl = vmspace->pgtbl;
#endif
    BUG_ON(pgtbl == NULL);
    l0_ptp = (ptp_t *)remove_pcid(pgtbl);
    l1_ptp = NULL;
    l2_ptp = NULL;
    l3_ptp = NULL;

    total_page_cnt = len / PAGE_SIZE + (((len % PAGE_SIZE) > 0) ? 1 : 0);

    while (total_page_cnt > 0) {
        // l0
        ret = get_next_ptp(l0_ptp, 0, va, &l1_ptp, &pte, false);
        if (ret != 0)
            printk("ret: %d\n", ret);
        BUG_ON(ret != 0);

        // l1
        ret = get_next_ptp(l1_ptp, 1, va, &l2_ptp, &pte, false);
        if (ret != 0)
            printk("ret: %d\n", ret);
        BUG_ON(ret != 0);

        // l2
        ret = get_next_ptp(l2_ptp, 2, va, &l3_ptp, &pte, false);
        BUG_ON(ret != 0);

        // l3
        // step-1: get the index of pte
        pte_index = GET_L3_INDEX(va);
        for (i = pte_index; i < PTP_ENTRIES; ++i) {
            entry = &(l3_ptp->ent[i]);
            /* set domain id (pkey) */
            pte_set_domain(entry, domain_id);

            va += PAGE_SIZE;
            total_page_cnt -= 1;
            if (total_page_cnt == 0)
                break;
        }
    }

    return 0;
}

#endif
