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
#include <mm/shm.h>
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
#include <irq/irq.h>
#include <mm/page_table_func.h>
#include <object/recycle.h>
#include <sched/sched.h>
#ifdef DSM_ENABLED
#include <dsm/dsm-single.h>
#include <dsm/cxl_reclaim.h>
#include <mm/remote_free.h>
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

/*
 * How many pages one Case 2.3 fault may migrate: the faulting page plus up to
 * PGFAULT_READAHEAD_MAX - 1 virtually contiguous pages still owned by the same
 * remote machine.  1 disables read-ahead entirely.
 *
 * Deliberately separate from POLLING_TLB_BATCH_MAX.  That constant sizes
 * entries[] inside struct polling_kernel_req_flush_tlb_batch, which is part of
 * the byte-exact shared-memory polling ABI between the kernel and the polling
 * server (it is defined a second time in user/system-servers/polling/polling.h
 * and validated in polling_resp.c); changing it desynchronizes the two sides
 * silently.  This constant only bounds how much of that array we fill, so
 * varying it leaves every shared struct byte-identical.
 *
 * Override at build time, e.g. from kernel/CMakeLists.txt:
 *   target_compile_definitions(${kernel_target} PRIVATE PGFAULT_READAHEAD_MAX=1)
 */
#ifndef PGFAULT_READAHEAD_MAX
#define PGFAULT_READAHEAD_MAX POLLING_TLB_BATCH_MAX
#endif
#if PGFAULT_READAHEAD_MAX < 1
#error "PGFAULT_READAHEAD_MAX must be at least 1 (1 disables read-ahead)"
#endif
#if PGFAULT_READAHEAD_MAX > POLLING_TLB_BATCH_MAX
#error "PGFAULT_READAHEAD_MAX must not exceed POLLING_TLB_BATCH_MAX"
#endif

#if defined(DSM_ENABLED) && defined(MULTI_PAGETABLE_ENABLED)
/* Back off repeated faults while a bounded migration transaction completes. */
#define CXL_FAULT_RETRY_BASE_NS 50000ULL
#define CXL_FAULT_RETRY_MAX_NS  1000000ULL
#define CXL_FAULT_RETRY_RESET_NS 10000000ULL

struct cxl_fault_retry_state {
    struct thread *thread;
    vaddr_t fault_addr;
    u64 last_ns;
    u8 shift;
};

/* Kernel-local state: never extend struct thread or another shared DSM ABI. */
static struct cxl_fault_retry_state cxl_fault_retries[PLAT_CPU_NUM];

static void cxl_fault_retry_timer_cb(struct thread *thread)
{
    thread->thread_ctx->state = TS_TO_SCHED;
    BUG_ON(sched_enqueue(thread));
}

static inline void reset_cxl_fault_retry(vaddr_t fault_addr)
{
    struct thread *thread = current_thread;
    struct cxl_fault_retry_state *state =
            &cxl_fault_retries[smp_get_cpu_id()];

    if (thread && state->thread == thread && state->fault_addr == fault_addr) {
        state->thread = NULL;
        state->fault_addr = 0;
        state->last_ns = 0;
        state->shift = 0;
    }
}

static void __attribute__((noreturn))
schedule_cxl_fault_retry(vaddr_t fault_addr)
{
    struct thread *thread = current_thread;
    struct cxl_fault_retry_state *state =
            &cxl_fault_retries[smp_get_cpu_id()];
    struct timespec retry;
    u64 delay_ns;
    u64 now;

    BUG_ON(!thread);
    now = plat_get_mono_time();
    if (state->thread != thread || state->fault_addr != fault_addr
        || now < state->last_ns
        || now - state->last_ns > CXL_FAULT_RETRY_RESET_NS) {
        state->thread = thread;
        state->fault_addr = fault_addr;
        state->shift = 0;
    }
    delay_ns = CXL_FAULT_RETRY_BASE_NS
               << MIN(state->shift, 5);
    delay_ns = MIN(delay_ns, CXL_FAULT_RETRY_MAX_NS);
    if (state->shift < 5)
        state->shift++;
    state->last_ns = now;
    retry.tv_sec = 0;
    retry.tv_nsec = delay_ns;
    dsm_cxl_note_fault_fallback();
    lock(&thread->sleep_state.queue_lock);
    BUG_ON(enqueue_sleeper(thread, &retry, cxl_fault_retry_timer_cb));
    thread->thread_ctx->state = TS_WAITING;

    /*
     * Select the next thread before releasing queue_lock.  The timer callback
     * cannot enqueue this thread until its kernel stack is on the way out,
     * which closes the wake-before-schedule race.
     */
    sched();
    unlock(&thread->sleep_state.queue_lock);
    eret_to_thread(switch_context());
    BUG("scheduled CXL fault retry returned\n");
}
#endif

/* Enable page fault statistics and debug logging */
/* Define PGFAULT_STATS_DEBUG to enable detailed statistics printing */
// #define PGFAULT_STATS_DEBUG

#ifdef MULTI_PAGETABLE_ENABLED
#ifdef PGFAULT_STATS_DEBUG
#define PGFAULT_STATS_RECENT_COUNT 1000  /* Store last 1000 samples */

/* Circular buffer for recent samples */
struct pgfault_case_recent_stats {
    u64 cycles_buffer[PGFAULT_STATS_RECENT_COUNT];
    u64 write_index;  /* Current write position */
    u64 count;        /* Actual number of samples (max PGFAULT_STATS_RECENT_COUNT) */
};

/* Statistics for page fault handling cases (only case 2.1, 2.2, 2.3) */
struct pgfault_case_stats {
    struct pgfault_case_recent_stats case_migration_entry; /* case 2.1 */
    struct pgfault_case_recent_stats case2_wait;           /* case 2.2 */
    struct pgfault_case_recent_stats case2_migrate;         /* case 2.3 */
};

static struct pgfault_case_stats pgfault_stats = {0};
/* Total count of case 2.1 + 2.2 + 2.3; print when reaches 1000 */
#define PGFAULT_CASE2_PRINT_THRESHOLD 1000
static u64 pgfault_case2_total_count = 0;
static struct lock pgfault_case2_stats_lock;
static bool pgfault_case2_stats_lock_initialized = false;

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

static void reset_case2_stats(void);
static void maybe_print_and_reset_case2_stats(void);

/* Add sample for case 2.1/2.2/2.3; when total reaches 1000, print stats and reset */
static void add_sample_case2(struct pgfault_case_recent_stats *stats, u64 cycles)
{
    add_sample(stats, cycles);
    if (atomic_fetch_add_64(&pgfault_case2_total_count, 1) + 1 >= PGFAULT_CASE2_PRINT_THRESHOLD)
        maybe_print_and_reset_case2_stats();
}
#endif /* PGFAULT_STATS_DEBUG */

#ifdef PGFAULT_STATS_DEBUG
static void reset_case2_stats(void)
{
    pgfault_stats.case_migration_entry.write_index = 0;
    pgfault_stats.case_migration_entry.count = 0;
    pgfault_stats.case2_wait.write_index = 0;
    pgfault_stats.case2_wait.count = 0;
    pgfault_stats.case2_migrate.write_index = 0;
    pgfault_stats.case2_migrate.count = 0;
    pgfault_case2_total_count = 0;
}

static void maybe_print_and_reset_case2_stats(void)
{
    if (!pgfault_case2_stats_lock_initialized) {
        lock_init(&pgfault_case2_stats_lock);
        pgfault_case2_stats_lock_initialized = true;
    }
    if (try_lock(&pgfault_case2_stats_lock) != 0)
        return;
    if (pgfault_case2_total_count >= PGFAULT_CASE2_PRINT_THRESHOLD) {
        print_pgfault_stats();
        reset_case2_stats();
    }
    unlock(&pgfault_case2_stats_lock);
}
#endif /* PGFAULT_STATS_DEBUG */

#ifdef PGFAULT_STATS_DEBUG
/* Print latency in microseconds (ns / 1000) */
static void print_latency_us(u64 avg_ns, u64 max_ns, u64 min_ns, u64 avg_cycles)
{
    printk("  latency: avg %llu us (avg %llu cycles), max %llu us, min %llu us\n",
           avg_ns / 1000ULL, avg_cycles, max_ns / 1000ULL, min_ns / 1000ULL);
}
#endif

void print_pgfault_stats(void)
{
#ifdef PGFAULT_STATS_DEBUG
    extern u64 cur_freq;
    u64 freq_ns = 0;
    if (cur_freq > 0) {
        freq_ns = cur_freq / 1000000000ULL;
        if (freq_ns == 0) freq_ns = 1;
    } else {
        freq_ns = 2400;
    }

    printk("=== Page Fault Case 2.1/2.2/2.3 Statistics (total %d reached) ===\n",
           PGFAULT_CASE2_PRINT_THRESHOLD);
    printk("CPU Frequency: %llu Hz (%llu cycles/ns)\n", cur_freq, freq_ns);

    u64 avg_cycles, max_cycles, min_cycles;

    /* Case 2.1: migration entry wait */
    printk("Case 2.1 (migration entry wait): count=%llu\n",
           pgfault_stats.case_migration_entry.count);
    if (pgfault_stats.case_migration_entry.count > 0) {
        calc_recent_stats(&pgfault_stats.case_migration_entry,
                         &avg_cycles, &max_cycles, &min_cycles);
        u64 avg_ns = avg_cycles / freq_ns;
        u64 max_ns = max_cycles / freq_ns;
        u64 min_ns = min_cycles / freq_ns;
        print_latency_us(avg_ns, max_ns, min_ns, avg_cycles);
    }

    /* Case 2.2: wait for migration */
    printk("Case 2.2 (wait for migration): count=%llu\n", pgfault_stats.case2_wait.count);
    if (pgfault_stats.case2_wait.count > 0) {
        calc_recent_stats(&pgfault_stats.case2_wait, &avg_cycles, &max_cycles, &min_cycles);
        u64 avg_ns = avg_cycles / freq_ns;
        u64 max_ns = max_cycles / freq_ns;
        u64 min_ns = min_cycles / freq_ns;
        print_latency_us(avg_ns, max_ns, min_ns, avg_cycles);
    }

    /* Case 2.3: trigger migration */
    printk("Case 2.3 (trigger migration): count=%llu\n", pgfault_stats.case2_migrate.count);
    if (pgfault_stats.case2_migrate.count > 0) {
        calc_recent_stats(&pgfault_stats.case2_migrate, &avg_cycles, &max_cycles, &min_cycles);
        u64 avg_ns = avg_cycles / freq_ns;
        u64 max_ns = max_cycles / freq_ns;
        u64 min_ns = min_cycles / freq_ns;
        print_latency_us(avg_ns, max_ns, min_ns, avg_cycles);
    }

    printk("===================================\n");
#endif
}

/*
 * DSM_MIGRATE_STATS: cumulative, per-machine accounting of what the DSM page
 * fault path actually did, so a placement configuration can be described by
 * measurement rather than by its cmake variables.
 *
 * Deliberately not PGFAULT_STATS_DEBUG: that one keeps a 1000-sample ring per
 * case, resets it whenever it prints, and reports latency percentiles.  It
 * answers "how expensive is a migration"; these counters answer "how many
 * migrations did this configuration cause at all", which needs monotonic
 * totals that survive to the end of the run.  Both can be on at once.
 *
 * The dump is emitted from the fault path itself rather than from a timer, so
 * it needs no new hook: one line per machine at most every DMS_DUMP_CYCLES,
 * printed by whichever CPU faults first after the interval expires.  The
 * consequence is that the totals stop advancing when faulting stops, so the
 * last line of a log is up to one interval short of the true final value --
 * fine at these magnitudes, and it also makes the tail of the run visible as
 * a line that stops changing.
 *
 * Enable from kernel/CMakeLists.txt:
 *   target_compile_definitions(${kernel_target} PRIVATE DSM_MIGRATE_STATS)
 */
#ifdef DSM_MIGRATE_STATS
/* ~1e9 cycles is 0.3-0.5 s on this testbed; exactness does not matter. */
#define DMS_DUMP_CYCLES 1000000000ULL

static volatile s64 dms_alloc;      /* first touch: fresh page committed here */
static volatile s64 dms_direct;     /* Case 1: already shared/local, just map */
static volatile s64 dms_c21;        /* Case 2.1: waited on a migration entry */
static volatile s64 dms_c22;        /* Case 2.2: waited on another thread */
static volatile s64 dms_c23;        /* Case 2.3: migrations this machine ran */
static volatile s64 dms_c23_pages;  /* pages those migrations actually pulled */
static volatile s64 dms_c23_raced;  /* Case 2.3 that found the page moved */
static volatile s64 dms_c23_cycles; /* cycles spent inside Case 2.3 */
static volatile s64 dms_c22_cycles; /* cycles spent waiting in Case 2.2 */
static volatile s64 dms_next_dump;

#define DMS_ADD(counter, n) atomic_fetch_add_64(&(counter), (s64)(n))
#define DMS_INC(counter)    DMS_ADD(counter, 1)

static void dms_tick(void)
{
    u64 now = get_cycles();
    s64 next = atomic_load_64((s64 *)&dms_next_dump);

    if ((s64)now < next)
        return;
    /*
     * Claim the interval before printing.  A CPU that loses the race skips
     * this round rather than queueing behind it: the dump is a sample, and
     * blocking a page fault on a serial console write would be the one way
     * this instrumentation could change what it measures.
     */
    if (!atomic_bool_compare_exchange_64(
                (s64 *)&dms_next_dump, next, (s64)now + (s64)DMS_DUMP_CYCLES))
        return;

    printk("[DMS] m=%d alloc=%lld direct=%lld c21=%lld c22=%lld c23=%lld "
           "c23pg=%lld c23raced=%lld c23cyc=%lld c22cyc=%lld\n",
           CUR_MACHINE_ID,
           atomic_load_64((s64 *)&dms_alloc),
           atomic_load_64((s64 *)&dms_direct),
           atomic_load_64((s64 *)&dms_c21),
           atomic_load_64((s64 *)&dms_c22),
           atomic_load_64((s64 *)&dms_c23),
           atomic_load_64((s64 *)&dms_c23_pages),
           atomic_load_64((s64 *)&dms_c23_raced),
           atomic_load_64((s64 *)&dms_c23_cycles),
           atomic_load_64((s64 *)&dms_c22_cycles));
}
#else
#define DMS_ADD(counter, n) do { } while (0)
#define DMS_INC(counter)    do { } while (0)
#define dms_tick()          do { } while (0)
#endif /* DSM_MIGRATE_STATS */

/* Wait until migration completes */
static void migration_entry_wait(pte_t *pte, struct vmspace *vmspace, vaddr_t fault_addr)
{
    /* Release locks before waiting to avoid deadlock with TLB flush IPI handlers */
    unlock(&vmspace->pgtbl_lock);
    read_unlock(&vmspace->vmspace_lock);

#if defined(DSM_ENABLED) && defined(MULTI_PAGETABLE_ENABLED)
    UNUSED(pte);
    schedule_cxl_fault_retry(fault_addr);
#else
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
        
        /* A demotion may deliberately leave this machine's PTE unmapped. */
        if (!pte_check || !is_migration_entry(pte_check)) {
            /* Migration complete, locks are still held */
            return;
        }
        
        /* Migration not complete yet, release locks and continue waiting */
        unlock(&vmspace->pgtbl_lock);
        read_unlock(&vmspace->vmspace_lock);
    }
#endif
}
#endif

#ifdef MULTI_PAGETABLE_ENABLED
/* Result of trying to reserve a VA for migration. */
#define MIGRATING_VA_RESERVED  0 /* this thread now owns the migration */
#define MIGRATING_VA_BUSY      1 /* somebody else is already migrating it */

/*
 * Check if a VA is migrating and reserve it if not (atomic operation).
 *
 * Returns MIGRATING_VA_RESERVED when the caller owns the reservation (and must
 * eventually call remove_migrating_va), MIGRATING_VA_BUSY when another thread
 * already owns it (*sender_machine_id is then filled in), or -ENOMEM when the
 * reservation could not be recorded.  The last case must not be folded into
 * either of the others: reporting RESERVED without an entry silently drops the
 * mutual exclusion, and reporting BUSY sends the caller off to wait for a
 * migration that nobody is performing.
 */
static int check_and_add_migrating_va(struct vmspace *vmspace, vaddr_t va, mid_t *sender_machine_id)
{
    struct migrating_va_entry *entry;
    bool found = false;
    int ret;

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

    if (found) {
        ret = MIGRATING_VA_BUSY;
    } else {
        /* If not found, add it to the list */
        entry = kmalloc(sizeof(*entry), __MT_SHARED__);
        if (entry) {
            entry->va = va;
            entry->sender_machine_id = CUR_MACHINE_ID;
            init_list_head(&entry->list_node);
            list_add(&entry->list_node, &vmspace->migrating_va_list);
            // kinfo(ANSI_COLOR_RED "[MIGRATION] add migrating va 0x%lx to migrating list\n" ANSI_COLOR_RESET, va);
            ret = MIGRATING_VA_RESERVED;
        } else {
            ret = -ENOMEM;
        }
    }
    unlock(&vmspace->migrating_va_lock);

    return ret;
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
     * Sample the counters before taking any lock, so the dump can never be
     * the reason a lock is held longer.
     */
    dms_tick();
#if defined(DSM_ENABLED) && defined(MULTI_PAGETABLE_ENABLED)
    reap_pending_shm_migrations();
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

    if (pmo->type == PMO_DEVICE) {
        /*
         * vmspace_map_range() already mapped this whole vmr -- but only into
         * the page table of the machine that did the mapping, because
         * fill_page_table() writes get_vmspace_pgtbl(vmspace, CUR_MACHINE_ID).
         * Reaching a translation fault means this is a different machine
         * running the same vmspace (an IPC handler thread runs on the
         * caller's CPU, so the fs server's CXLFS mapping is touched from
         * every machine its clients run on), so just repeat the eager
         * mapping here.
         *
         * Device memory must not enter the migration logic below: its pa is a
         * BAR address, which belongs to neither the shared region nor any
         * machine's DRAM, so get_paddr_machine_id() would return
         * MACHINE_ID_INVALID and trip the BUG_ON after the Case-2.1 check.
         * Nothing needs migrating anyway: pmo->start is read from this
         * machine's own copy of the kernel-local cxlfs_pmos[], so it already
         * holds this machine's BAR address for the same physical region.
         *
         * fill_page_table() takes pgtbl_lock, which nests inside the
         * vmspace_lock still held here -- and vmr is only valid under it.
         */
        kwarn_once("%s: first PMO_DEVICE fault on this machine, vmr "
                   "[0x%lx, 0x%lx) pa 0x%lx -- mapping it locally\n",
                   __func__, vmr->start, vmr->start + vmr->size, pmo->start);
        ret = fill_page_table(vmspace, vmr);
        if (ret < 0)
            kwarn("%s: failed to map device vmr [0x%lx, 0x%lx): %d\n",
                  __func__, vmr->start, vmr->start + vmr->size, ret);
        goto out_unlock_vmspace;
    }

    /* A valid pmo, should handle page fault */
    perm = vmr->perm;

    /*
     * A write to a vmr the process is not allowed to write is a permission
     * violation, not something to resolve.  Without this the handler would
     * just re-install the same read-only PTE from vmr->perm and return 0,
     * the instruction would retry, and the CPU would spin in the fault
     * forever instead of the process being killed.
     *
     * Test vmr->perm, never the PTE: checkpointing write-protects pages for
     * dirty tracking via set_write_in_pgtbl(), which touches PTEs only and
     * runs exactly on the vmrs that do carry VMR_WRITE, so those faults must
     * still fall through and be resolved here.
     *
     * Only for an already-present page.  On the first touch of a read-only
     * mapping there is nothing mapped yet; let that fault install the
     * read-only PTE (PMO_FILE deliberately widens perm below) and catch the
     * write on the retry.
     */
    if (present && write && !(vmr->perm & VMR_WRITE)) {
        ret = -EPERM;
        goto out_unlock_vmspace;
    }

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

        /* Add mapping in the page table */
        lock(&vmspace->pgtbl_lock);
#ifdef MULTI_PAGETABLE_ENABLED
        void *pgtbl = get_vmspace_pgtbl(vmspace, CUR_MACHINE_ID);
        /*
         * Re-check PTE under pgtbl_lock: a concurrent migration
         * (sys_memcpy_and_flush_tlb) may have set a migration entry
         * on this VA between our get_page_from_pmo and acquiring the
         * lock. This happens when the page was mapped via
         * pgtbl_deep_copy without commit_page_to_pmo.
         */
        {
            pte_t *existing_pte = NULL;
            query_in_pgtbl(pgtbl, fault_addr, NULL, &existing_pte);
            if (existing_pte && is_migration_entry(existing_pte)) {
                free_pages(new_va);
                migration_entry_wait(existing_pte, vmspace, fault_addr);
                unlock(&vmspace->pgtbl_lock);
                read_unlock(&vmspace->vmspace_lock);
                return 0;
            }
            if (existing_pte && existing_pte->pte_4K.present) {
                free_pages(new_va);
                unlock(&vmspace->pgtbl_lock);
                goto out_unlock_vmspace;
            }
        }
#endif

        /*
         * Record the physical page in the radix tree:
         * the offset is used as index in the radix tree.
         * Moved after migration entry check to avoid corrupting
         * the PMO radix tree during a concurrent migration.
         */
        kdebug("commit: index: %ld, 0x%lx\n", index, pa);
        commit_page_to_pmo(pmo, index, pa);
        DMS_INC(dms_alloc);

#ifdef MULTI_PAGETABLE_ENABLED
        map_page_in_pgtbl(pgtbl, fault_addr, pa, perm, &pte);
#else
        map_page_in_pgtbl(vmspace->pgtbl, fault_addr, pa, perm, &pte);
#endif
        if (get_paddr_machine_id(pa) == MACHINE_ID_SHARED_MEMORY)
            cxlprof_live_mark_cxl(vmspace, fault_addr);
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

        /**
         * If MULTI_PAGETABLE is enabled, we need to map all
         * page tables to the shared memory.
         * Case1: the page is already on shared memory
         *        -- DO NOTHING! Directly map!
         * Case2: the page is not on shared memory
         *   Case2.1: the page is already being migrated by other thread,
         *            indicated by that the pte is a migration entry
         *            then wait until this page finish migration
         *   Case2.2: another thread tigger a page fault on this page,
         *            and uses `check_and_add_migrating_va` to mark
         *            this page as being migrated by this thread
         *   Case2.3: the page is not being migrated by other thread,
         *            then send message to the sender machine to migrate pages
         *        -- 2.1: memcpy the page to the shared memory.
         *        -- 2.2: remap the page table in old page table.
         *        -- 2.3: map the page to new page table.
         * Case3: the page is not on shared memory but belongs to
         *        PMO_DATA type, which is pre-defined in the PMO,
         *        and should be mapped in the page table here.
         */
#ifdef MULTI_PAGETABLE_ENABLED
        void *pgtbl = get_vmspace_pgtbl(vmspace, CUR_MACHINE_ID);
        query_in_pgtbl(pgtbl, fault_addr, &pa, &pte);
        /**
        * Case2.1: page is already during migration by other thread, 
        * then wait until this page finish migration
        *
        * e.g.，machine 0 - thread 0 migrate this page
        * => page table is changed to migration entry, then flush tlb
        * machine 0 - thread 1 also tigger a page fault, and wait here
        */
        if (pte && is_migration_entry(pte)) {
            paddr_t settled_pa = 0;
            pte_t *settled_pte = NULL;
#ifdef PGFAULT_STATS_DEBUG
            u64 start_cycles = get_cycles();
#endif
            // multipt_debug("[case2.1 migration wait]"
            //     "cpu %d found migration entry, fault_addr 0x%lx, waiting...\n", 
            //     smp_get_cpu_id(), fault_addr);
            /* Wait until migration completes */
            /* Note: migration_entry_wait will release and re-acquire locks internally */
            migration_entry_wait(pte, vmspace, fault_addr);
            /*
             * The migration entry is shared by both directions. Promotion
             * leaves a CXL mapping, while demotion may install owner DRAM or
             * remove this machine's mapping entirely. Classify the settled
             * PTE instead of blindly restoring the CXL bit after the wait.
             */
            query_in_pgtbl(pgtbl, fault_addr, &settled_pa, &settled_pte);
            if (settled_pte && settled_pte->pte_4K.present
                && get_paddr_machine_id(settled_pa)
                           == MACHINE_ID_SHARED_MEMORY)
                cxlprof_live_mark_cxl(vmspace, fault_addr);
            else
                cxlprof_live_mark_dram(vmspace, fault_addr);
            DMS_INC(dms_c21);
#ifdef PGFAULT_STATS_DEBUG
            u64 end_cycles = get_cycles();
            u64 cycles = end_cycles - start_cycles;
            add_sample_case2(&pgfault_stats.case_migration_entry, cycles);
#endif
            /* Page is already mapped after migration, no need to handle fault */
            /* Locks are still held by migration_entry_wait */
            unlock(&vmspace->pgtbl_lock);
            read_unlock(&vmspace->vmspace_lock);
            return 0;
        }
#ifdef DSM_ENABLED
        /*
         * Do not install a mapping for a page the demoter has already
         * snapshotted.  prepare_candidate() records every PTE that can reach
         * the CXL page and turns it into a migration entry before the
         * distributed TLB shootdown; a mapping added after that snapshot
         * escapes the shootdown and still points at the page when
         * finish_candidate() returns it to the buddy allocator, so the
         * process later reads recycled memory.  This runs under pgtbl_lock,
         * which prepare_candidate() also takes, so the two orders are
         * covered: if reclaim got here first the check sees it and the fault
         * is retried after demotion; if this check got here first,
         * prepare_candidate() observes the new PTE and includes it.
         */
        if (dsm_cxl_mapping_in_transition(pmo, index, pa)) {
            unlock(&vmspace->pgtbl_lock);
            read_unlock(&vmspace->vmspace_lock);
            schedule_cxl_fault_retry(fault_addr);
        }
#endif
        /* NOTE!!: should not define machine_id variable here,
        it will cause the error when using CUR_MACHINE_ID */
        int mid = get_paddr_machine_id(pa);
        paddr_t new_pa = pa;
        BUG_ON(mid == MACHINE_ID_INVALID);
#ifdef PGFAULT_STATS_DEBUG
        u64 case2_start_cycles = 0;
#endif
#ifdef DSM_MIGRATE_STATS
        /*
         * Own clock rather than reusing case2_start_cycles: the two flags are
         * independent, and this one has to include the whole Case 2 path.
         */
        u64 dms_entry_cycles = 0;
#endif
        if (mid != MACHINE_ID_SHARED_MEMORY && mid != CUR_MACHINE_ID) {
#ifdef PGFAULT_STATS_DEBUG
            case2_start_cycles = get_cycles();
#endif
#ifdef DSM_MIGRATE_STATS
            dms_entry_cycles = get_cycles();
#endif
            /* Check if this VA is already being migrated by another thread */
            /* Release locks before checking/adding to avoid deadlock */
            unlock(&vmspace->pgtbl_lock);
            read_unlock(&vmspace->vmspace_lock);
            
            /*
             * Admit capacity before publishing migrating-VA ownership.  If
             * admission misses, no peer can mistake this fault for a
             * migration which it must wait for.
             */
            if (dsm_cxl_reserve_resident_pages(1) != 0) {
                return -ENOMEM;
            }

            /* Atomically check and reserve; see check_and_add_migrating_va(). */
            mid_t sender_mid = MACHINE_ID_INVALID;
            int reserve_ret =
                    check_and_add_migrating_va(vmspace, fault_addr, &sender_mid);
            if (reserve_ret < 0) {
                /*
                 * The reservation could not be recorded, so this migration
                 * cannot be made exclusive against other threads.  Nothing has
                 * been changed anywhere.  Treat a persistent metadata OOM as
                 * a normal user fault failure instead of retrying forever.
                 */
                kwarn_once("[MIGRATION] cannot reserve a VA for migration\n");
                dsm_cxl_cancel_resident_pages(1);
                return -ENOMEM;
            }
            if (reserve_ret == MIGRATING_VA_BUSY) {
                /*
                 * Another fault owns the migration.  Do not spin on its
                 * marker or remote RPC: its completion will make the next
                 * scheduled fault take the direct-map path.
                 */
                BUG_ON(sender_mid == MACHINE_ID_INVALID);
                DMS_INC(dms_c22);
                DMS_ADD(dms_c22_cycles, get_cycles() - dms_entry_cycles);
                dsm_cxl_cancel_resident_pages(1);
                schedule_cxl_fault_retry(fault_addr);
            } else {
                /**
                 * Case2.3: the page is not being migrated by other thread,
                 * then send message to the sender machine to migrate pages
                 * Successfully added to migrating list, now perform migration
                 *
                 * Re-check: Another machine may have completed migration and
                 * removed from list before we ran check_and_add. Re-acquire
                 * locks and verify the data-source machine's page table.
                 * If the page is already in shared memory, just map it.
                 */
                multipt_debug("[case2.3 migration trigger]"
                    "cpu %d trigger case2, fault_addr: 0x%lx, machine_id: %d\n", 
                    smp_get_cpu_id(), fault_addr, mid);

                /* Re-check: page might already be migrated by another machine */
                read_lock(&vmspace->vmspace_lock);
                lock(&vmspace->pgtbl_lock);
                void *src_pgtbl = get_vmspace_pgtbl(vmspace, mid);
                paddr_t recheck_pa = 0;
                pte_t *recheck_pte = NULL;
                int recheck_ret = query_in_pgtbl(src_pgtbl, fault_addr, &recheck_pa, &recheck_pte);
                if (recheck_ret == 0 && recheck_pte && recheck_pte->pte_4K.present
                    && !is_migration_entry(recheck_pte)
                    && get_paddr_machine_id(recheck_pa) == MACHINE_ID_SHARED_MEMORY) {
                    /* Page already migrated by another machine, just map it */
                    new_pa = recheck_pa;
                    pgtbl = get_vmspace_pgtbl(vmspace, CUR_MACHINE_ID);
                    map_page_in_pgtbl(pgtbl, fault_addr, new_pa, perm, &pte);
                    cxlprof_live_mark_cxl(vmspace, fault_addr);
                    unlock(&vmspace->pgtbl_lock);
                    read_unlock(&vmspace->vmspace_lock);
                    dsm_cxl_cancel_resident_pages(1);
                    remove_migrating_va(vmspace, fault_addr);
                    DMS_INC(dms_c23_raced);
                    DMS_ADD(dms_c23_cycles, get_cycles() - dms_entry_cycles);
#ifdef PGFAULT_STATS_DEBUG
                    u64 case2_end_cycles = get_cycles();
                    u64 case2_cycles = case2_end_cycles - case2_start_cycles;
                    add_sample_case2(&pgfault_stats.case2_migrate, case2_cycles);
#endif
                    multipt_debug("[case2.3 skipped] va 0x%lx already migrated by another machine, just map\n",
                        fault_addr);
                    reset_cxl_fault_retry(fault_addr);
                    return 0;
                }
                /*
                 * IMPORTANT: if the source mapping changed concurrently, we must
                 * migrate from the rechecked PA instead of the stale PA captured
                 * before releasing locks. Otherwise we may copy wrong page data.
                 */
                if (recheck_ret == 0 && recheck_pte && recheck_pte->pte_4K.present
                    && !is_migration_entry(recheck_pte)) {
                    pa = recheck_pa;
                    mid = get_paddr_machine_id(pa);
                    BUG_ON(mid == MACHINE_ID_INVALID);

                    /*
                     * Owner changed while we were racing with another migration.
                     * If it is now local/shared, direct map and finish.
                     */
                    if (mid == CUR_MACHINE_ID || mid == MACHINE_ID_SHARED_MEMORY) {
                        new_pa = pa;
                        pgtbl = get_vmspace_pgtbl(vmspace, CUR_MACHINE_ID);
                        map_page_in_pgtbl(pgtbl, fault_addr, new_pa, perm, &pte);
                        if (mid == MACHINE_ID_SHARED_MEMORY)
                            cxlprof_live_mark_cxl(vmspace, fault_addr);
                        unlock(&vmspace->pgtbl_lock);
                        read_unlock(&vmspace->vmspace_lock);
                        dsm_cxl_cancel_resident_pages(1);
                        remove_migrating_va(vmspace, fault_addr);
                        DMS_INC(dms_c23_raced);
                        DMS_ADD(dms_c23_cycles, get_cycles() - dms_entry_cycles);
#ifdef PGFAULT_STATS_DEBUG
                        u64 case2_end_cycles = get_cycles();
                        u64 case2_cycles = case2_end_cycles - case2_start_cycles;
                        add_sample_case2(&pgfault_stats.case2_migrate, case2_cycles);
#endif
                        multipt_debug("[case2.3 owner changed] va 0x%lx now maps to pa 0x%lx on mid %d, direct map\n",
                                      fault_addr, new_pa, mid);
                        reset_cxl_fault_retry(fault_addr);
                        return 0;
                    }
                }
                /*
                 * Snapshot the VMR bound while the vmspace lock still covers
                 * vmr; the read-ahead loop below runs unlocked and must not
                 * dereference it there.
                 */
                vaddr_t vmr_end = vmr->start + vmr->size;
                unlock(&vmspace->pgtbl_lock);
                read_unlock(&vmspace->vmspace_lock);
                /*
                 * Migrate a run of pages, not just the faulting one.
                 *
                 * Each request costs the source machine two all-CPU TLB
                 * shootdown IPIs (see sys_memcpy_and_flush_tlb), which dwarf
                 * the 4 KiB copy; batching amortizes both those shootdowns and
                 * the round trip over the whole run.  PGFAULT_READAHEAD_MAX
                 * bounds speculation by each fault.  The CXL reclaim path may
                 * later move cold pages back to their origin DRAM pages.
                 */
                struct polling_tlb_batch_entry batch[POLLING_TLB_BATCH_MAX];
                struct cxl_track_op track_ops[POLLING_TLB_BATCH_MAX];
                u64 batch_count;
                u64 bi;
                u64 read_ahead_reserved = 0;
                u64 read_ahead_used = 0;
                vaddr_t next_va;
                void *dst_va;
                int migrate_ret;
                bool reclaim_enabled = dsm_cxl_reclaim_enabled();
                bool tracking_failed[POLLING_TLB_BATCH_MAX] = { false };

                dst_va = get_pages(0, __MT_SHARED__);
                if (dst_va == NULL) {
                    dsm_cxl_cancel_resident_pages(1);
                    remove_migrating_va(vmspace, fault_addr);
                    return -ENOMEM;
                }
                batch[0].src_pa = pa;
                batch[0].dst_pa = virt_to_phys(dst_va);
                batch[0].fault_va = fault_addr; /* already page aligned above */
                batch_count = 1;

                if (PGFAULT_READAHEAD_MAX > 1) {
                    u64 pages_left = (vmr_end - fault_addr) / PAGE_SIZE;

                    if (pages_left > 1) {
                        read_ahead_reserved = MIN(
                                (u64)PGFAULT_READAHEAD_MAX - 1,
                                pages_left - 1);
                        if (dsm_cxl_reserve_resident_pages(
                                    read_ahead_reserved) != 0)
                            read_ahead_reserved = 0;
                    }
                }

                for (next_va = batch[0].fault_va + PAGE_SIZE;
                     read_ahead_used < read_ahead_reserved
                         && batch_count < PGFAULT_READAHEAD_MAX
                         && next_va < vmr_end;
                     next_va += PAGE_SIZE) {
                    mid_t owner = MACHINE_ID_INVALID;
                    paddr_t cand_pa = 0;
                    pte_t *cand_pte = NULL;

                    if (check_and_add_migrating_va(vmspace, next_va, NULL)
                        != MIGRATING_VA_RESERVED)
                        break;

                    read_lock(&vmspace->vmspace_lock);
                    lock(&vmspace->pgtbl_lock);
                    if (query_in_pgtbl(get_vmspace_pgtbl(vmspace, mid), next_va,
                                       &cand_pa, &cand_pte) == 0
                        && cand_pte && cand_pte->pte_4K.present
                        && !is_migration_entry(cand_pte))
                        owner = get_paddr_machine_id(cand_pa);
                    unlock(&vmspace->pgtbl_lock);
                    read_unlock(&vmspace->vmspace_lock);

                    /* Stop at the first page that is not still owned by mid. */
                    if (owner != mid) {
                        remove_migrating_va(vmspace, next_va);
                        break;
                    }
                    dst_va = get_pages(0, __MT_SHARED__);
                    if (dst_va == NULL) {
                        remove_migrating_va(vmspace, next_va);
                        break;
                    }
                    batch[batch_count].src_pa = cand_pa;
                    batch[batch_count].dst_pa = virt_to_phys(dst_va);
                    batch[batch_count].fault_va = next_va;
                    batch_count++;
                    read_ahead_used++;
                }
                if (read_ahead_reserved > read_ahead_used)
                    dsm_cxl_cancel_resident_pages(
                            read_ahead_reserved - read_ahead_used);

                migrate_ret = migrate_pages_to_shm_batch(
                        mid, vmspace, pmo, index, batch, batch_count, perm);
                if (migrate_ret == -EINPROGRESS)
                    schedule_cxl_fault_retry(fault_addr);
                if (migrate_ret != 0) {
                    /*
                     * The remote machine applied nothing (the batch syscall is
                     * all-or-nothing), so every dst page is still uninitialized
                     * and the source mappings are untouched.  Mapping them here
                     * would hand the process blank memory in place of its data.
                     *
                     * Give the pages back and drop the reservations instead.
                     * That releases any Case 2.2 waiter, which then finds this
                     * machine's PTE unchanged and re-faults, exactly like this
                     * thread does when it returns 0 here.  Reporting an error
                     * would kill the process (or panic, for a kernel-mode
                     * fault) over what is usually a transient remote failure.
                     */
                    kwarn_once("[MIGRATION] a remote promotion batch failed; "
                               "using scheduled fault retries\n");
                    for (bi = 0; bi < batch_count; bi++) {
                        dsm_cxl_cancel_resident_pages(1);
                        free_pages((void *)phys_to_virt(batch[bi].dst_pa));
                        remove_migrating_va(vmspace, batch[bi].fault_va);
                    }
                    schedule_cxl_fault_retry(fault_addr);
                }
                /* Re-acquire locks */
                read_lock(&vmspace->vmspace_lock);
                lock(&vmspace->pgtbl_lock);

                /* Get the page table of the current machine */
                pgtbl = get_vmspace_pgtbl(vmspace, CUR_MACHINE_ID);

                /* Map every migrated page and prepare one batched FIFO publish. */
                for (bi = 0; bi < batch_count; bi++) {
                    map_page_in_pgtbl(pgtbl, batch[bi].fault_va,
                                      batch[bi].dst_pa, perm, &pte);
                    cxlprof_live_mark_cxl(vmspace, batch[bi].fault_va);

                    track_ops[bi].cxl_pa = batch[bi].dst_pa;
                    track_ops[bi].origin_pa = batch[bi].src_pa;
                    track_ops[bi].pmo = pmo;
                    track_ops[bi].pmo_index = index + bi;
                    track_ops[bi].vmspace = vmspace;
                    track_ops[bi].va = batch[bi].fault_va;
                    track_ops[bi].perm = perm;
                    track_ops[bi].owner_mid = mid;
                    track_ops[bi].speculative = bi != 0;
                    track_ops[bi].result = -EOPNOTSUPP;
                }

                if (reclaim_enabled) {
                    int batch_track_ret =
                            dsm_cxl_track_pages(track_ops, batch_count);

                    if (batch_track_ret < 0) {
                        for (bi = 0; bi < batch_count; bi++)
                            track_ops[bi].result = batch_track_ret;
                    }
                }

                for (bi = 0; bi < batch_count; bi++) {
                    int track_ret = track_ops[bi].result;

                    if (track_ret < 0) {
                        tracking_failed[bi] = true;
                        if (reclaim_enabled)
                            kwarn_once(
                                    "[CXL_RECLAIM] Failed to track a "
                                    "migrated page; releasing origin DRAM\n");
                    }
                }

                /* Unlock the page table and vmspace lock */
                unlock(&vmspace->pgtbl_lock);
                read_unlock(&vmspace->vmspace_lock);

                for (bi = 0; bi < batch_count; bi++) {
                    if (!tracking_failed[bi])
                        continue;
                    dsm_cxl_cancel_resident_pages(1);
                    free_machine_page(batch[bi].src_pa);
                }

                /*
                 * Release the reservations only once every mapping is in
                 * place: a Case 2.2 waiter on this machine returns as soon as
                 * the VA leaves the migrating list and does no mapping of its
                 * own when the sender is the current machine.
                 */
                for (bi = 0; bi < batch_count; bi++)
                    remove_migrating_va(vmspace, batch[bi].fault_va);
                new_pa = batch[0].dst_pa;

                DMS_INC(dms_c23);
                DMS_ADD(dms_c23_pages, batch_count);
                DMS_ADD(dms_c23_cycles, get_cycles() - dms_entry_cycles);
#ifdef PGFAULT_STATS_DEBUG
                u64 case2_end_cycles = get_cycles();
                u64 case2_cycles = case2_end_cycles - case2_start_cycles;
                add_sample_case2(&pgfault_stats.case2_migrate, case2_cycles);
#endif
                reset_cxl_fault_retry(fault_addr);
                return 0;
            }
        }

        /* Case1: Direct map (shared memory or local) - no stats */
        DMS_INC(dms_direct);
        map_page_in_pgtbl(pgtbl, fault_addr, new_pa, perm, &pte);
        if (get_paddr_machine_id(new_pa) == MACHINE_ID_SHARED_MEMORY)
            cxlprof_live_mark_cxl(vmspace, fault_addr);
#else
        int mid = get_paddr_machine_id(pa);
        BUG_ON(mid != CUR_MACHINE_ID && mid != MACHINE_ID_SHARED_MEMORY);
        map_page_in_pgtbl(vmspace->pgtbl, fault_addr, pa, perm, &pte);
        if (mid == MACHINE_ID_SHARED_MEMORY)
            cxlprof_live_mark_cxl(vmspace, fault_addr);
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
#if defined(DSM_ENABLED) && defined(MULTI_PAGETABLE_ENABLED)
    if (ret == 0)
        reset_cxl_fault_retry(fault_addr);
#endif
    return ret;
}

#endif
