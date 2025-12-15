#include <common/types.h>
#include <common/kprint.h>
#include <common/lock.h>
#include <mm/mm.h>
#include <mm/vmspace.h>
#include <mm/kmalloc.h>

#include <arch/mm/page_table.h>
#ifdef DSM_ENABLED
#include <dsm/dsm-single.h>
#endif

/* the virtual address of kernel page table */
extern char CHCORE_PGD[];

/*
static void pte_set_user_bit(pte_t *entry)
{
        entry->pteval |= PAGE_USER;
}
*/

static void pte_clear_user_bit(pte_t *entry)
{
    entry->pteval &= ~PAGE_USER;
}

static void __arch_vmspace_init_single(void *pgtbl)
{
    ptp_t *ptp_k;
    ptp_t *ptp_u;
    pte_t *entry_k;
    pte_t *entry_u;
    int index;

    /*
     * We map the kernel space in the user pgtbl
     * but mark them as Supervisor (user cannot access).
     *
     * We use 1G huge page for the kernel address mapping.
     */

    /* Kernel root page table page */
    ptp_k = (ptp_t *)CHCORE_PGD;
    /* User process root page table page */
    ptp_u = (ptp_t *)pgtbl;

    /* Map the kernel code part in the user page table */
    index = GET_L0_INDEX(KCODE);
    BUG_ON(index != 511);
    /* The page table entry for the kernel code mapping */
    entry_k = &(ptp_k->ent[index]);
    /* The corresponding entry in the user page table */
    entry_u = &(ptp_u->ent[index]);
    /* Set the mapping in the user page table */
    entry_u->pteval = entry_k->pteval;
    /* Protect kernel form user */
    pte_clear_user_bit(entry_u);

    /* Map the kernel direct mapping part in the user page table */
    index = GET_L0_INDEX(KBASE);
    BUG_ON(index != 510);
    /* The page table entry for the kernel code mapping */
    entry_k = &(ptp_k->ent[index]);
    /* The corresponding entry in the user page table */
    entry_u = &(ptp_u->ent[index]);
    /* Setup the mappings in the user page table */
    entry_u->pteval = entry_k->pteval;
    /* Protect kernel form user */
    pte_clear_user_bit(entry_u);
}

/* Initialize the top page table page for a user-level process */
void arch_vmspace_init(struct vmspace *vmspace)
{
#ifdef MULTI_PAGETABLE_ENABLED
    /* Initialize page tables for all machines */
    for (int i = 0; i < CLUSTER_MAX_MACHINE_NUM; i++) {
        if (vmspace->pgtbl[i] != NULL) {
            __arch_vmspace_init_single(vmspace->pgtbl[i]);
            vmspace->pgtbl[i] = (void *)((u64)vmspace->pgtbl[i] | vmspace->pcid);
        }
    }
#else
    /* For non-DSM_ENABLED builds, initialize single page table */
    if (vmspace->pgtbl != NULL) {
        __arch_vmspace_init_single(vmspace->pgtbl);
        vmspace->pgtbl = (void *)((u64)vmspace->pgtbl | vmspace->pcid);
    }
#endif
}

struct vmspace *create_idle_vmspace(void)
{
    struct vmspace *idle_vmspace;

    idle_vmspace = kzalloc(sizeof(*idle_vmspace), __MT_PRIVATE__);
    /* Cannot use the CHCORE_PGD directly because its addr < IMG_END */
#ifdef MULTI_PAGETABLE_ENABLED
    for (int i = 0; i < CLUSTER_MAX_MACHINE_NUM; i++) {
#if defined USE_NVM && defined USE_DRAM
        idle_vmspace->pgtbl[i] = get_dram_pages(0);
#else
        idle_vmspace->pgtbl[i] = get_pages(0, __MT_PRIVATE__);
#endif
        BUG_ON(idle_vmspace->pgtbl[i] == NULL);
        memset(idle_vmspace->pgtbl[i], 0, PAGE_SIZE);
    }
    arch_vmspace_init(idle_vmspace);
#else
#if defined USE_NVM && defined USE_DRAM
    idle_vmspace->pgtbl = get_dram_pages(0);
#else
    idle_vmspace->pgtbl = get_pages(0, __MT_PRIVATE__);
#endif
    BUG_ON(idle_vmspace->pgtbl == NULL);
    memset(idle_vmspace->pgtbl, 0, PAGE_SIZE);

    __arch_vmspace_init_single(idle_vmspace->pgtbl);
#endif

    return idle_vmspace;
}

/* Get page table for a specific machine id */
void *get_vmspace_pgtbl(struct vmspace *vmspace, mid_t machine_id)
{
#ifdef MULTI_PAGETABLE_ENABLED
    if (machine_id >= 0 && machine_id < CLUSTER_MACHINE_NUM) {
        return vmspace->pgtbl[machine_id];
    }
    BUG("Failed to get page table for machine %d\n", machine_id);
#else
    return vmspace->pgtbl;
#endif
}

/* Change vmspace to the target one */
void switch_vmspace_to(struct vmspace *vmspace)
{
#ifdef MULTI_PAGETABLE_ENABLED
    void *pgtbl = get_vmspace_pgtbl(vmspace, CUR_MACHINE_ID);
    if (pgtbl == NULL) {
        BUG("Failed to get page table for machine %d\n", CUR_MACHINE_ID);
    }
    /* CR3: the lower 12-bit represent PCID */
    u64 pcid = vmspace->pcid;
    pgtbl = (void *)((u64)pgtbl | pcid);
    set_page_table(virt_to_phys(pgtbl));
#else
    void *pgtbl = (void *)((u64)vmspace->pgtbl | vmspace->pcid);
    set_page_table(virt_to_phys(pgtbl));
#endif
}