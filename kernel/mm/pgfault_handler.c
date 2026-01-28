#include <common/macro.h>
#include <common/util.h>
#include <common/list.h>
#include <common/errno.h>
#include <common/kprint.h>
#include <common/types.h>
#include <common/lock.h>
#include <lib/printk.h>
#include <mm/vmspace.h>
#include <mm/kmalloc.h>
#include <mm/buddy.h>
#include <mm/mm.h>
#include <mm/nvm.h>
#include <mm/vmspace.h>
#include <mm/nvm.h>
#include <mm/rmap.h>
#include <arch/mmu.h>
#include <object/thread.h>
#include <object/cap_group.h>
#include <object/user_fault.h>
#include <sched/context.h>
#include <arch/sync.h>
#include <arch/time.h>
#include <irq/timer.h>
#include <mm/page.h>
#include <arch/mm/page_table.h>
#include <arch/mm/tlb.h>
#include <mm/page_table_func.h>
#include <object/recycle.h>
#ifdef DSM_ENABLED
#include <dsm/dsm-single.h>
#include <lib/fw_cfg.h>
#include <drivers/ivshmem.h>
#endif
#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
#include <ckpt/hot_pages_tracker.h>
#include <ckpt/ckpt.h>
#endif

/* Policy on-demand: only mapping the faulting address */
#define ONDEMAND 0

/*
 * Not implemented now.
 * Policy pre-fault: mapping the serveral continous pages in advance.
 */
#define PREFAULT 1

#define PGFAULT_POLICY ONDEMAND

/* Enable page fault statistics and debug logging */
/* Define PGFAULT_STATS_DEBUG to enable detailed statistics printing */
#define PGFAULT_STATS_DEBUG

#ifdef MULTI_PAGETABLE_ENABLED
#ifdef PGFAULT_STATS_DEBUG
#define PGFAULT_STATS_RECENT_COUNT 1000  /* Store last 1000 samples */

/* Circular buffer for recent samples */
struct pgfault_case_recent_stats {
    u64 cycles_buffer[PGFAULT_STATS_RECENT_COUNT];
    u64 write_index;  /* Current write position */
    u64 count;        /* Actual number of samples (max PGFAULT_STATS_RECENT_COUNT) */
};

/* Statistics for page fault handling cases */
struct pgfault_case_stats {
    struct pgfault_case_recent_stats case_migration_entry;
    struct pgfault_case_recent_stats case2_wait;
    struct pgfault_case_recent_stats case2_migrate;
    struct pgfault_case_recent_stats case1_direct_map;
};

static struct pgfault_case_stats pgfault_stats = {0};

/* Add a sample to circular buffer */
static void add_sample(struct pgfault_case_recent_stats *stats, u64 cycles)
{
    stats->cycles_buffer[stats->write_index] = cycles;
    stats->write_index = (stats->write_index + 1) % PGFAULT_STATS_RECENT_COUNT;
    if (stats->count < PGFAULT_STATS_RECENT_COUNT)
        stats->count++;
}

/* Calculate average, max, min from recent samples */
static void calc_recent_stats(struct pgfault_case_recent_stats *stats, 
                              u64 *avg, u64 *max, u64 *min)
{
    if (stats->count == 0) {
        *avg = *max = *min = 0;
        return;
    }
    
    u64 sum = 0;
    *max = 0;
    *min = ~0ULL;  /* Maximum u64 value */
    
    for (u64 i = 0; i < stats->count; i++) {
        u64 cycles = stats->cycles_buffer[i];
        sum += cycles;
        if (cycles > *max) *max = cycles;
        if (cycles < *min) *min = cycles;
    }
    
    *avg = sum / stats->count;
}

/* Forward declaration */
void print_pgfault_stats(void);

/* Timer for periodic statistics printing */
#define PGFAULT_STATS_PRINT_INTERVAL_MS 5000  /* Print every 5 seconds */
static u64 pgfault_stats_last_print_time = 0;
static bool pgfault_stats_timer_enabled = true;
static struct lock pgfault_stats_print_lock;
static bool pgfault_stats_lock_initialized = false;
#endif /* PGFAULT_STATS_DEBUG */

#ifdef PGFAULT_STATS_DEBUG
/* Check and print statistics periodically */
static void check_and_print_pgfault_stats(void)
{
    if (!pgfault_stats_timer_enabled)
        return;
    
    /* Initialize lock on first use */
    if (!pgfault_stats_lock_initialized) {
        lock_init(&pgfault_stats_print_lock);
        pgfault_stats_lock_initialized = true;
    }
    
    u64 current_time_ns = plat_get_mono_time();
    u64 interval_ns = PGFAULT_STATS_PRINT_INTERVAL_MS * 1000000ULL; /* Convert ms to ns */
    
    /* Use try_lock to avoid blocking page fault handling */
    if (try_lock(&pgfault_stats_print_lock) == 0) {
        if (current_time_ns - pgfault_stats_last_print_time >= interval_ns) {
            pgfault_stats_last_print_time = current_time_ns;
            print_pgfault_stats();
        }
        unlock(&pgfault_stats_print_lock);
    }
}
#endif /* PGFAULT_STATS_DEBUG */

void print_pgfault_stats(void)
{
#ifdef PGFAULT_STATS_DEBUG
    extern u64 cur_freq;
    /* cur_freq is in Hz (cycles per second), convert to cycles per nanosecond */
    u64 freq_ns = 0;
    if (cur_freq > 0) {
        freq_ns = cur_freq / 1000000000ULL; /* cycles per nanosecond */
        if (freq_ns == 0) freq_ns = 1; /* avoid division by zero */
    } else {
        /* Fallback: assume 2.4 GHz if cur_freq is not initialized */
        freq_ns = 2400; /* 2.4 GHz = 2.4 cycles per nanosecond */
    }
    
    printk("=== Page Fault Case Statistics (Recent %d samples) ===\n", 
           PGFAULT_STATS_RECENT_COUNT);
    printk("CPU Frequency: %llu Hz (assuming %llu cycles/ns for conversion)\n", 
           cur_freq, freq_ns);
    
    u64 avg_cycles, max_cycles, min_cycles;
    
    if (pgfault_stats.case_migration_entry.count > 0) {
        calc_recent_stats(&pgfault_stats.case_migration_entry, 
                         &avg_cycles, &max_cycles, &min_cycles);
        u64 avg_ns = avg_cycles / freq_ns;
        u64 max_ns = max_cycles / freq_ns;
        u64 min_ns = min_cycles / freq_ns;
        printk("Case: Migration Entry Wait\n");
        printk("  Recent Samples: %llu\n", pgfault_stats.case_migration_entry.count);
        printk("  Avg: %llu cycles (%llu ns), Max: %llu cycles (%llu ns), Min: %llu cycles (%llu ns)\n", 
               avg_cycles, avg_ns, max_cycles, max_ns, min_cycles, min_ns);
    }
    
    if (pgfault_stats.case2_wait.count > 0) {
        calc_recent_stats(&pgfault_stats.case2_wait, 
                         &avg_cycles, &max_cycles, &min_cycles);
        u64 avg_ns = avg_cycles / freq_ns;
        u64 max_ns = max_cycles / freq_ns;
        u64 min_ns = min_cycles / freq_ns;
        printk("Case2: Wait for Migration (already migrating)\n");
        printk("  Recent Samples: %llu\n", pgfault_stats.case2_wait.count);
        printk("  Avg: %llu cycles (%llu ns), Max: %llu cycles (%llu ns), Min: %llu cycles (%llu ns)\n", 
               avg_cycles, avg_ns, max_cycles, max_ns, min_cycles, min_ns);
    }
    
    if (pgfault_stats.case2_migrate.count > 0) {
        calc_recent_stats(&pgfault_stats.case2_migrate, 
                         &avg_cycles, &max_cycles, &min_cycles);
        u64 avg_ns = avg_cycles / freq_ns;
        u64 max_ns = max_cycles / freq_ns;
        u64 min_ns = min_cycles / freq_ns;
        printk("Case2: Trigger Migration\n");
        printk("  Recent Samples: %llu\n", pgfault_stats.case2_migrate.count);
        printk("  Avg: %llu cycles (%llu ns), Max: %llu cycles (%llu ns), Min: %llu cycles (%llu ns)\n", 
               avg_cycles, avg_ns, max_cycles, max_ns, min_cycles, min_ns);
    }
    
    if (pgfault_stats.case1_direct_map.count > 0) {
        calc_recent_stats(&pgfault_stats.case1_direct_map, 
                         &avg_cycles, &max_cycles, &min_cycles);
        u64 avg_ns = avg_cycles / freq_ns;
        u64 max_ns = max_cycles / freq_ns;
        u64 min_ns = min_cycles / freq_ns;
        printk("Case1: Direct Map (shared memory or local)\n");
        printk("  Recent Samples: %llu\n", pgfault_stats.case1_direct_map.count);
        printk("  Avg: %llu cycles (%llu ns), Max: %llu cycles (%llu ns), Min: %llu cycles (%llu ns)\n", 
               avg_cycles, avg_ns, max_cycles, max_ns, min_cycles, min_ns);
    }
    
    printk("===================================\n");
#endif
}

/* Wait until migration completes */
static void migration_entry_wait(pte_t *pte, struct vmspace *vmspace, vaddr_t fault_addr)
{
    /* Release locks before waiting to avoid deadlock with TLB flush IPI handlers */
    unlock(&vmspace->pgtbl_lock);
    read_unlock(&vmspace->vmspace_lock);

    extern void handle_ipi(void);
    while (1) {
        CPU_PAUSE();
        handle_ipi();

        /* Re-acquire locks to check PTE */
        read_lock(&vmspace->vmspace_lock);
        lock(&vmspace->pgtbl_lock);
        
        /* Re-query PTE as it may have changed */
        void *pgtbl = get_vmspace_pgtbl(vmspace, CUR_MACHINE_ID);
        paddr_t pa_check;
        pte_t *pte_check = NULL;
        query_in_pgtbl(pgtbl, fault_addr, &pa_check, &pte_check);
        
        /* Check if migration is complete (PTE is present and not a migration entry) */
        if (pte_check && pte_check->pte_4K.present && !is_migration_entry(pte_check)) {
            /* Migration complete, locks are still held */
            return;
        }
        
        /* Migration not complete yet, release locks and continue waiting */
        unlock(&vmspace->pgtbl_lock);
        read_unlock(&vmspace->vmspace_lock);
    }
}
#endif

#ifdef MULTI_PAGETABLE_ENABLED
/* Check if a virtual address is currently being migrated */
static bool is_va_migrating(struct vmspace *vmspace, vaddr_t va, mid_t *sender_machine_id)
{
    struct migrating_va_entry *entry;
    bool found = false;
    
    lock(&vmspace->migrating_va_lock);
    for_each_in_list(entry, struct migrating_va_entry, list_node, &vmspace->migrating_va_list) {
        // printk(ANSI_COLOR_RED "[MIGRATION] find migrating va: 0x%lx\n" ANSI_COLOR_RESET, entry->va);
        if (entry->va == va) {
            if (sender_machine_id) {    
                *sender_machine_id = entry->sender_machine_id;
            }
            found = true;
            break;
        }
    }
    unlock(&vmspace->migrating_va_lock);
    
    return found;
}

/* Check if a VA is migrating and add it if not (atomic operation) */
/* Returns true if VA was already migrating, false if successfully added */
static bool check_and_add_migrating_va(struct vmspace *vmspace, vaddr_t va, mid_t *sender_machine_id)
{
    struct migrating_va_entry *entry;
    bool found = false;
    
    lock(&vmspace->migrating_va_lock);
    /* First check if already in the list */
    for_each_in_list(entry, struct migrating_va_entry, list_node, &vmspace->migrating_va_list) {
        if (entry->va == va) {
            if (sender_machine_id) {
                *sender_machine_id = entry->sender_machine_id;
            }
            found = true;
            break;
        }
    }
    
    /* If not found, add it to the list */
    if (!found) {
        entry = kmalloc(sizeof(*entry), __MT_SHARED__);
        if (entry) {
            entry->va = va;
            entry->sender_machine_id = CUR_MACHINE_ID;
            init_list_head(&entry->list_node);
            list_add(&entry->list_node, &vmspace->migrating_va_list);
            // kinfo(ANSI_COLOR_RED "[MIGRATION] add migrating va 0x%lx to migrating list\n" ANSI_COLOR_RESET, va);
        }
    }
    unlock(&vmspace->migrating_va_lock);
    
    return found;
}

/* Remove a virtual address from the migrating list */
void remove_migrating_va(struct vmspace *vmspace, vaddr_t va)
{
    struct migrating_va_entry *entry, *tmp;
    
    lock(&vmspace->migrating_va_lock);
    for_each_in_list_safe(entry, tmp, list_node, &vmspace->migrating_va_list) {
        if (entry->va == va) {
            list_del(&entry->list_node);
            kfree(entry);
            // kinfo(ANSI_COLOR_RED "[MIGRATION] remove migrating va 0x%lx from migrating list\n" ANSI_COLOR_RESET, va);
            break;
        }
    }
    unlock(&vmspace->migrating_va_lock);
}

/* Wait until migration completes for a specific VA and PTE is mapped */
static void wait_for_migration_complete(struct vmspace *vmspace, vaddr_t va, paddr_t *out_pa, mid_t sender_machine_id)
{
    void *pgtbl;
    paddr_t pa;
    pte_t *pte;
    int ret;
    
    /* First wait for the migrating entry to be removed */
    while (is_va_migrating(vmspace, va, NULL)) {
        CPU_PAUSE();
    }

    /* Then wait for the PTE to be mapped on the sender machine */
    while (1) {
        read_lock(&vmspace->vmspace_lock);
        lock(&vmspace->pgtbl_lock);
        pgtbl = get_vmspace_pgtbl(vmspace, sender_machine_id);
        ret = query_in_pgtbl(pgtbl, va, &pa, &pte);
        
        if (ret == 0 && pte && pte->pte_4K.present 
            && !is_migration_entry(pte)) {
            /* PTE is properly mapped, migration is complete */
            if (out_pa) {
                *out_pa = pa;
            }
            unlock(&vmspace->pgtbl_lock);
            read_unlock(&vmspace->vmspace_lock);
            return;
        }
        
        unlock(&vmspace->pgtbl_lock);
        read_unlock(&vmspace->vmspace_lock);
        CPU_PAUSE();
    }
}
#endif

#if PGFAULT_POLICY == ONDEMAND

#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
/* add_pte_patch_to_pool: when trigger write, track pages's pte and page struct
 */
void add_pte_patch_to_pool(struct vmspace *vmspace, pte_t *pte,
                           struct page *page)
{
    struct pte_patch_pool *pool, *new_pool;

    pool = (struct pte_patch_pool *)vmspace->pte_patch_pool;
    if (pool) {
        /* set next pte entry */
        /* check if vmspace lock is already get? */
        // lock(&vmspace->pte_patch_lock);
        pool->array[pool->count] =
                (struct pte_patch_pool_entry){.pte = pte, .page = page};
        // printk("add_pte_patch_to_pool: page=%p\n", page);
        if (++pool->count >= MAX_ENTRY) {
            /* Full, alloc a new pool */
            new_pool = (struct pte_patch_pool *)create_patch_pool();
            new_pool->next = pool;
            vmspace->pte_patch_pool = new_pool;
        }
        // unlock(&vmspace->pte_patch_lock);
    } else {
        // kinfo("[ERR] empty pte_patch_pool\n");
    }
}

#ifdef REPORT
u64 patch_page_num = 0;
#endif
#ifdef REPORT_RUNTIME
extern u64 pf_count;
extern u64 pf_tot_time;
#endif
#endif /* CHCORE_SLS */

int map_page_in_pgtbl(void *pgtbl, vaddr_t va, paddr_t pa, vmr_prop_t flags,
                      pte_t **out_pte);
int handle_trans_fault(struct vmspace *vmspace, vaddr_t fault_addr, int present,
                       int write)
{
#ifdef MULTI_PAGETABLE_ENABLED
#ifdef PGFAULT_STATS_DEBUG
    /* Check and print statistics periodically */
    check_and_print_pgfault_stats();
#endif
#endif
#ifdef REPORT_RUNTIME
    DECLTMR;
    start();
#endif
    struct vmregion *vmr;
    struct pmobject *pmo;
    struct page *page;
    paddr_t pa;
    u64 offset;
    u64 index;
    int ret = 0;
    pte_t *pte; /* out pte */
    vmr_prop_t perm;
#if defined CHCORE_SLS
    int ckpt_ret = 0;
#if defined(OMIT_PF) && !defined(PMO_CHECKSUM)
    UNUSED(page);
#endif
#endif
    /*
     * Grab lock here.
     * Because two threads (in same process) on different cores
     * may fault on the same page, so we need to prevent them
     * from adding the same mapping twice.
     */
    read_lock(&vmspace->vmspace_lock);

    vmr = find_vmr_for_va(vmspace, fault_addr);
    if (vmr == NULL) {
        kinfo("handle_trans_fault: no vmr found for va 0x%lx!\n", fault_addr);
        read_unlock(&vmspace->vmspace_lock);
        return -ENOMAPPING;
    }

    pmo = vmr->pmo;

    if (pmo->type > PMO_TYPE_NR || pmo->type < 0) {
        kinfo("handle_trans_fault: faulting vmr->pmo->type (pmo type %d at 0x%lx)\n",
              vmr->pmo->type,
              fault_addr);
        kinfo("Currently, this pmo type should not trigger pgfaults\n");
        kprint_vmr(vmspace);
        ret = -ENOMAPPING;
        goto out_unlock_vmspace;
    }

    if (pmo->type == PMO_FORBID) {
        kinfo("handle_trans_fault: forbidden memory access (pmo->type is PMO_FORBID).\n");
        BUG_ON(1);
        sys_exit_group(-1);
        ret = -EINVAL;
        goto out_unlock_vmspace;
    }

    /* A valid pmo, should handle page fault */
    perm = vmr->perm;

    /* Get the offset in the pmo for faulting addr */
    offset = ROUND_DOWN(fault_addr, PAGE_SIZE) - vmr->start;

    /* Boundary check */
    if ((offset >= pmo->size) && (pmo->type == PMO_FILE)) {
        kwarn_once(
                "%s (out-of-range writing) offset 0x%lx, pmo->size 0x%lx, FILE\n",
                __func__,
                offset,
                pmo->size);
        /*
         * FIXME: we simply allow it now by adding new pages.
         * For PMO_FILE, users can mmap a memory that is larger
         * than the file size. If they accesses bytes beyond
         * file size, SIGBUS should be triggered on LInux.
         *
         * TODO: why setting all the perm here?
         */
        perm = VMR_READ | VMR_WRITE | VMR_EXEC;
    } else {
        BUG_ON(offset >= pmo->size);
    }

    /* Get the index in the pmo radix for faulting addr */
    index = offset / PAGE_SIZE;

    fault_addr = ROUND_DOWN(fault_addr, PAGE_SIZE);

    pa = get_page_from_pmo(pmo, index);

    /* PMO_FILE fault means user fault */
    if (pmo->type == PMO_FILE && !pa) {
        /* pa != 0 means this fault is cause by ckpt/restore */
#ifdef CHCORE_ENABLE_FMAP
        read_unlock(&vmspace->vmspace_lock);
        handle_user_fault(pmo, fault_addr);
        /* One short-cut exit */
        BUG("Should never be here!\n");
#else
        ret = -EINVAL;
        break;
#endif
    }

    if (pa == 0) {
        /* Not committed before. Then, allocate the physical page. */
#ifdef MULTI_PAGETABLE_ENABLED
        /* If MULTI_PAGETABLE is enabled, we need to allocate
         * a private page for the new mapping. Only when another
         * machine also want to access this page, the page will
         * be shared. */
        void *new_va = get_pages(0, pmo->mm_type);
        /* Otherwise, allocate according to the pmo's mm_type */
#else
        void *new_va = get_pages(0, pmo->mm_type);
#endif
        BUG_ON(new_va == NULL);

        pa = virt_to_phys(new_va);
        BUG_ON(pa == 0);

        /* Clear to 0 for the newly allocated page */
        memset(new_va, 0, PAGE_SIZE);

        /*
         * Record the physical page in the radix tree:
         * the offset is used as index in the radix tree
         */
        kdebug("commit: index: %ld, 0x%lx\n", index, pa);
        commit_page_to_pmo(pmo, index, pa);

        /* Add mapping in the page table */
        lock(&vmspace->pgtbl_lock);
#ifdef MULTI_PAGETABLE_ENABLED
        void *pgtbl = get_vmspace_pgtbl(vmspace, CUR_MACHINE_ID);
        map_page_in_pgtbl(pgtbl, fault_addr, pa, perm, &pte);
#else
        map_page_in_pgtbl(vmspace->pgtbl, fault_addr, pa, perm, &pte);
#endif
        unlock(&vmspace->pgtbl_lock);

#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
        /* do not persist pages belong to external sync pmo */
        if (is_external_sync_pmo(pmo))
            goto out_unlock_vmspace;
#ifdef OMIT_BENCHMARK
        /* omit track page fault of benchmarks */
        if (is_benchmark_vmspace(vmspace)) {
            goto out_unlock_vmspace;
        }
#endif
#ifndef OMIT_PF
        page = virt_to_page(new_va);
        if ((vmspace->flags & VM_FLAG_PRESERVE) && !is_external_sync_pmo(pmo)) {
#ifdef PMO_CHECKSUM
            page->ckpt_version_number = get_current_ckpt_version() + 1;
#endif
#ifndef OMIT_BENCHMARK
#ifdef CHCORE_SSI_SLS
            ckpt_dsm_page(pmo, new_va, index);
#else
            ckpt_nvm_page(pmo, new_va, index);
#endif
#endif
            add_pte_patch_to_pool(vmspace, (pte_t *)pte, page);
        }
#endif
#endif /* CHCORE_SLS */
    } else {
        /**
         * pa != 0: the faulting address has be committed a    \
         * physical page.                                      \
         *                                                     \
         * For concurrent page faults:                         \
         *                                                     \
         * When type is PMO_ANONYM, the later faulting threads \
         * of the process do not need to modify the page       \
         * table because a previous faulting thread will do    \
         * that. (This is always true for the same process)    \
         * However, if one process map an anonymous pmo for    \
         * another process (e.g., main stack pmo), the faulting
         * thread (e.g, in the new process) needs to update    \
         * its page table. \
         * So, for simplicity, we just update the page table.  \
         * Note that adding the same mapping is harmless.      \
         *                                                     \
         * When type is PMO_SHM, the later faulting threads    \
         * needs to add the mapping in the page table.         \
         * Repeated mapping operations are harmless.           \
         */

        /* For PMO_FILE, we simply set all the perm now. */
        if (pmo->type == PMO_FILE) {
            perm = VMR_READ | VMR_WRITE | VMR_EXEC;
        }

#if defined CHCORE_SLS && !defined(OMIT_PF)
        if ((vmspace->flags & VM_FLAG_PRESERVE) && !write
            && !is_external_sync_pmo(pmo)) {
            /* Read preserved page, map as read-only */
            perm &= ~VMR_WRITE;
        }
#endif /* CHCORE_SLS */

        /* handle CoW when a process is forked */
#ifdef CHCORE_FORK_ENABLED
#ifdef MULTI_PAGETABLE_ENABLED
#error "Multi-PAGETABLE_ENABLED is not supported for CoW"
#endif
        if (!is_shared_pmo(pmo)) {
            if (is_continuous_pmo(pmo)) {
                page = virt_to_page((void *)phys_to_virt(pmo->start));

                lock(&page->lock);
                if (page->ref_cnt > 1) {
                    void *new_va = kmalloc(pmo->size, pmo->mm_type);
                    if (new_va == NULL) {
                        unlock(&page->lock);
                        return -ENOMEM;
                    }

                    memcpy(new_va, (void *)phys_to_virt(pmo->start), pmo->size);
                    pmo->start = virt_to_phys(new_va);
                    /* new pa */
                    pa = pmo->start + index * PAGE_SIZE;

                    lock(&vmspace->pgtbl_lock);
                    void *pgtbl = vmspace->pgtbl;
                    if ((vmspace->flags & VM_FLAG_PRESERVE)) {
                        map_range_in_pgtbl(pgtbl,
                                           vmr->start,
                                           pmo->start,
                                           pmo->size,
                                           perm & (~VMR_WRITE));
                    } else {
                        map_range_in_pgtbl(
                                pgtbl, vmr->start, pmo->start, pmo->size, perm);
                    }
                    unlock(&vmspace->pgtbl_lock);

                    flush_tlbs(vmspace, vmr->start, vmr->size);
                    atomic_fetch_sub_64(&page->ref_cnt, 1);
                }
                unlock(&page->lock);
            } else {
                int cow = false;
                page = virt_to_page((void *)phys_to_virt(pa));
                lock(&page->lock);
                if (page->ref_cnt > 1) {
                    void *new_va = get_pages(0, pmo->mm_type);
                    if (new_va == NULL) {
                        unlock(&page->lock);
                        return -ENOMEM;
                    }

                    pagecpy_nt(new_va, (void *)phys_to_virt(pa));
                    /* new pa */
                    pa = virt_to_phys(new_va);

                    lock(&vmspace->pgtbl_lock);
                    map_page_in_pgtbl(
                            vmspace->pgtbl, fault_addr, pa, perm, &pte);
                    unlock(&vmspace->pgtbl_lock);

                    flush_tlbs(vmspace, fault_addr, PAGE_SIZE);
                    atomic_fetch_sub_64(&page->ref_cnt, 1);
                    cow = true;
                }
                unlock(&page->lock);
                if (cow) {
                    commit_page_to_pmo(pmo, index, pa);
                    unlock(&page->lock);
                    return 0;
                }
            }
        }
#endif /* CHCORE_FORK_ENABLED */

        /* Add mapping in the page table */
        pte_t *pte = NULL;
        lock(&vmspace->pgtbl_lock);
#ifdef MULTI_PAGETABLE_ENABLED
        void *pgtbl = get_vmspace_pgtbl(vmspace, CUR_MACHINE_ID);
        /* First handle the migration entry case */
        query_in_pgtbl(pgtbl, fault_addr, &pa, &pte);
        if (pte && is_migration_entry(pte)) {
#ifdef PGFAULT_STATS_DEBUG
            u64 start_cycles = get_cycles();
            kdebug("cpu %d found migration entry, fault_addr 0x%lx, waiting...\n", 
                smp_get_cpu_id(), fault_addr);
#endif
            /* Wait until migration completes */
            /* Note: migration_entry_wait will release and re-acquire locks internally */
            migration_entry_wait(pte, vmspace, fault_addr);
#ifdef PGFAULT_STATS_DEBUG
            u64 end_cycles = get_cycles();
            u64 cycles = end_cycles - start_cycles;
            /* Update statistics */
            add_sample(&pgfault_stats.case_migration_entry, cycles);
#endif
            
            /* Page is already mapped after migration, no need to handle fault */
            /* Locks are still held by migration_entry_wait */
            unlock(&vmspace->pgtbl_lock);
            read_unlock(&vmspace->vmspace_lock);
            return 0;
        }
        /**
         * If MULTI_PAGETABLE is enabled, we need to map all
         * page tables to the shared memory.
         * Case1: the page is already on shared memory
         *        -- DO NOTHING! Directly map!
         * Case2: the page is not on shared memory
         *        -- 2.1: memcpy the page to the shared memory.
         *        -- 2.2: remap the page table in old page table.
         *        -- 2.3: map the page to new page table.
         * Case3: the page is not on shared memory but belongs to
         *        PMO_DATA type, which is pre-defined in the PMO,
         *        and should be mapped in the page table here.
         */
        /* NOTE!!: should not define machine_id variable here,
        it will cause the error when using CUR_MACHINE_ID */
        int mid = get_paddr_machine_id(pa);
        paddr_t new_pa = pa;
        BUG_ON(mid == MACHINE_ID_INVALID);
        /* Case2 */
#ifdef PGFAULT_STATS_DEBUG
        u64 case2_start_cycles = 0;
#endif
        if (mid != MACHINE_ID_SHARED_MEMORY && mid != CUR_MACHINE_ID) {
#ifdef PGFAULT_STATS_DEBUG
            case2_start_cycles = get_cycles();
            kdebug("cpu %d entering case2 branch, fault_addr: 0x%lx, mid: %d\n", 
                smp_get_cpu_id(), fault_addr, mid);
#endif
            /* Check if this VA is already being migrated by another thread */
            /* Release locks before checking/adding to avoid deadlock */
            unlock(&vmspace->pgtbl_lock);
            read_unlock(&vmspace->vmspace_lock);
            
            /* Atomically check and add: if already migrating, 
             * returns true with sender_machine_id.
             * If not migrating, adds it to the list and returns false.
            */
            mid_t sender_mid = MACHINE_ID_INVALID;
            if (check_and_add_migrating_va(vmspace, fault_addr, &sender_mid)) {
                /* The sender machine ID should not be invalid */
                BUG_ON(sender_mid == MACHINE_ID_INVALID);

                /* VA was already being migrated by another thread */
                /* Wait for the migration to complete and PTE to be mapped */
#ifdef PGFAULT_STATS_DEBUG
                kdebug("cpu %d va 0x%lx is already being migrated, waiting...\n", 
                    smp_get_cpu_id(), fault_addr);
#endif

                wait_for_migration_complete(vmspace, fault_addr, &new_pa, sender_mid);

                /* If the sender machine is not the current machine,
                 * we need to map the page to the page table directly.
                 * Re-acquire locks before modifying page table.
                 */
                if (sender_mid != CUR_MACHINE_ID) {
                    /* Re-acquire locks */
                    read_lock(&vmspace->vmspace_lock);
                    lock(&vmspace->pgtbl_lock);

                    /* Get the page table of the current machine */
                    pgtbl = get_vmspace_pgtbl(vmspace, CUR_MACHINE_ID);

                    /* Map the page to the page table */
                    BUG_ON(get_paddr_machine_id(new_pa) != MACHINE_ID_SHARED_MEMORY);
                    map_page_in_pgtbl(pgtbl, fault_addr, new_pa, perm, &pte);

                    /* Unlock the page table and vmspace lock */
                    unlock(&vmspace->pgtbl_lock);
                    read_unlock(&vmspace->vmspace_lock);
                }
                
                /* Update statistics for Case2 Wait */
#ifdef PGFAULT_STATS_DEBUG
                u64 case2_end_cycles = get_cycles();
                u64 case2_cycles = case2_end_cycles - case2_start_cycles;
                add_sample(&pgfault_stats.case2_wait, case2_cycles);
#endif
                
                return 0;
            } else {
                /* Successfully added to migrating list, now perform migration */
#ifdef PGFAULT_STATS_DEBUG
                kdebug("cpu %d trigger case2, fault_addr: 0x%lx, machine_id: %d\n", 
                    smp_get_cpu_id(), fault_addr, mid);
#endif
                
                /* Send message to the sender machine to migrate pages */
                new_pa = migrate_pages_to_shm(mid, vmspace, pa, PAGE_SIZE, fault_addr);
                
                /* Re-acquire locks */
                read_lock(&vmspace->vmspace_lock);
                lock(&vmspace->pgtbl_lock);

                /* Get the page table of the current machine */
                pgtbl = get_vmspace_pgtbl(vmspace, CUR_MACHINE_ID);

                /* Map the page to the page table */
                map_page_in_pgtbl(pgtbl, fault_addr, new_pa, perm, &pte);

                /* Unlock the page table and vmspace lock */
                unlock(&vmspace->pgtbl_lock);
                read_unlock(&vmspace->vmspace_lock);

                /* Remove from migrating list */
                remove_migrating_va(vmspace, fault_addr);

                /* Update statistics for Case2 Migrate */
#ifdef PGFAULT_STATS_DEBUG
                u64 case2_end_cycles = get_cycles();
                u64 case2_cycles = case2_end_cycles - case2_start_cycles;
                add_sample(&pgfault_stats.case2_migrate, case2_cycles);

                kdebug("cpu %d migrate pages on machine %d completed\n", smp_get_cpu_id(), mid);
#endif
                
                return 0;
            }
        }

        /* Case1: Direct map (shared memory or local) */
#ifdef PGFAULT_STATS_DEBUG
        u64 case1_start_cycles = get_cycles();
#endif
        map_page_in_pgtbl(pgtbl, fault_addr, new_pa, perm, &pte);
#ifdef PGFAULT_STATS_DEBUG
        u64 case1_end_cycles = get_cycles();
        u64 case1_cycles = case1_end_cycles - case1_start_cycles;
        /* Update statistics for Case1 */
        add_sample(&pgfault_stats.case1_direct_map, case1_cycles);
#endif
        
#else
        int mid = get_paddr_machine_id(pa);
        BUG_ON(mid != CUR_MACHINE_ID && mid != MACHINE_ID_SHARED_MEMORY);
        map_page_in_pgtbl(vmspace->pgtbl, fault_addr, pa, perm, &pte);
#endif
        unlock(&vmspace->pgtbl_lock);

#ifdef CHCORE_SLS
        /* do not persist pages belong to external sync pmo */
        if (is_external_sync_pmo(pmo))
            return 0;
#ifndef OMIT_PF
#ifdef OMIT_BENCHMARK
        /* omit track page fault of benchmarks */
        if (is_benchmark_vmspace(vmspace)) {
            return 0;
        }
#endif

        if (write && (vmspace->flags & VM_FLAG_PRESERVE)) {
            page = virt_to_page((void *)phys_to_virt(pa));
            BUG_ON(unlikely(!page));
            if (unlikely(get_page_type(page) != NVM_PAGE)) {
                /* Dram page will be marked as unwritable after fork */
                unlock(&page->lock);
                return -ENOMEM;
            }
#ifndef OMIT_MEMCPY
            /* copy page to ckpt_page */
            if (pmo != page->pmo) {
                page->pmo = pmo;
            }

            ckpt_ret = ckpt_nvm_page(pmo, (void *)phys_to_virt(pa), index);

#endif /* OMIT_MEMCPY */
            /* Add pte patch */
            add_pte_patch_to_pool(vmspace, pte, page);
#ifdef HYBRID_MEM
            if (!ckpt_ret)
                track_access(page);
#else
            UNUSED(ckpt_ret);
#endif /* HYBRID_MEM */
#ifdef REPORT_RUNTIME
            pf_count++;
            pf_tot_time += stop();
            LOG("[ckpt=%llu] [page fault count] page=%p, pte=%p\n",
                CKPT_VERSION_NUMBER,
                page,
                pte);
#endif
        }
#else
        UNUSED(ckpt_ret);
#endif
#endif /* CHCORE_SLS */
    }

out_unlock_vmspace:
    read_unlock(&vmspace->vmspace_lock);
    return ret;
}

#endif
