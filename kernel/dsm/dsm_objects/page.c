#include <mm/vmspace.h>
#include <mm/page.h>

#include <arch/mm/page_table.h>
#include <arch/mm/tlb.h>

#include "../dsm_tiering.h"

extern int is_pte_dirty(pte_t *entry);
extern void clear_pte_dirty(pte_t *entry);
extern pte_t get_and_clear_pte(pte_t *pte);
extern int remap_page_in_pgtbl(pte_t *entry, paddr_t new_pa);
extern int set_pte_flags(pte_t *entry, vmr_prop_t flags, int kind);
extern int query_in_pgtbl(void *pgtbl, vaddr_t va, paddr_t *pa, pte_t **entry);
extern void flush_tlb_local_and_remote(struct vmspace *vmspace, vaddr_t va, size_t len);

/* demote a page in transaction mode */
/* Update page table (Tx mode akin to Nomad@OSDI'24) */
int dsm_demote_page(struct vmspace *vmspace, void *dst_va, void *src_va, bool retry)
{
    pte_t *pte;
    pte_t old_pte_value;
    query_in_pgtbl(vmspace->pgtbl, (vaddr_t)src_va, NULL, &pte);

retry:
    // TODO: check pte should not be changed here
    old_pte_value = *pte;

    // 1. clear the dirty bit of the page
    clear_pte_dirty(pte);

    // 2. issue a TLB shootdown for the page
    // TODO: find CPU that might have this page
    flush_tlb_local_and_remote(vmspace, (vaddr_t)src_va, PAGE_SIZE);

    // 3. copy the page
    pagecpy(dst_va, src_va);

    // 4. get_and_clear pte
    old_pte_value = get_and_clear_pte(pte);

    // 5. issue TLB shootdown again
    flush_tlb_local_and_remote(vmspace, (vaddr_t)pte, sizeof(pte_t));

    // 6: check whether the page is dirty again
    if (is_pte_dirty(pte)) {
        // 7: commit the transaction
        remap_page_in_pgtbl(pte, virt_to_phys(dst_va));
        clear_pte_dirty(pte);
    } else {
        // TODO: something wrong, pte is invalid; should retry
        BUG_ON(pte->pte_4K.pfn == old_pte_value.pte_4K.pfn);
        // 8: unlike Nomad, always try again for migration
        if (retry) {
            goto retry;
        }
    }
            
    return 0;
}
