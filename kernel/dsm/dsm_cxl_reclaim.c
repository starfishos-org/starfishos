#include <dsm/cxl_reclaim.h>
#include <dsm/cxl_reclaim_policy.h>
#include <dsm/dsm-single.h>

#ifdef DSM_ENABLED

#include <arch/machine/smp.h>
#include <arch/mm/page_table.h>
#include <arch/mm/tlb.h>
#include <common/errno.h>
#include <common/kprint.h>
#include <common/macro.h>
#include <common/util.h>
#include <drivers/ivshmem.h>
#include <irq/timer.h>
#include <mm/buddy.h>
#include <mm/kmalloc.h>
#include <mm/mm.h>
#include <mm/page_table_func.h>
#include <mm/vmspace.h>
#include <object/object.h>

#define CXL_PMO_MAPPING_UNINITIALIZED 0
#define CXL_PMO_MAPPING_INITIALIZING  1
#define CXL_PMO_MAPPING_READY         2

#define CXL_ORIGIN_NOT_RELEASED 0
#define CXL_ORIGIN_RELEASING    1
#define CXL_ORIGIN_RELEASED     2

/* The worker step and dedicated control mailbox form one batch ABI. */
_Static_assert(CXL_DEMOTE_MAX_BATCH == CXL_DEMOTE_WIRE_MAX_OPS,
               "CXL demote batch limits must stay in sync");
_Static_assert((int)CXL_DEMOTE_PHASE_FLUSH
                          == (int)CXL_DEMOTE_WIRE_FLUSH
               && (int)CXL_DEMOTE_PHASE_COPY
                          == (int)CXL_DEMOTE_WIRE_COPY
               && (int)CXL_DEMOTE_PHASE_FREE_ORIGIN
                          == (int)CXL_DEMOTE_WIRE_FREE_ORIGIN
               && (int)CXL_DEMOTE_PHASE_BITMAP_DRAM
                          == (int)CXL_DEMOTE_WIRE_BITMAP_DRAM,
               "CXL demote phase values must stay in sync");
_Static_assert((int)CXL_CONTROL_TXN_DEMOTE
                          == (int)CXL_CONTROL_WIRE_DEMOTE
               && (int)CXL_CONTROL_TXN_AGING
                          == (int)CXL_CONTROL_WIRE_AGING,
               "CXL control transaction values must stay in sync");

/* Upper bound on CLOCK entries examined by one aging pass. */
#define CXL_DEMOTE_SELECT_SCAN_LIMIT (8 * CXL_DEMOTE_MAX_BATCH)

/*
 * Sampling window (see the sample_cursor comment in dsm-single.h).
 *
 * The window has to be small enough that one pass costs far less than the
 * workload's reuse distance, and large enough that the cold pages it exposes
 * can keep the demoter's rate limit fed.  At CXL_DEMOTE_MAX_BATCH pages per
 * scan interval, 4096 pages is 64 scan intervals per pass.
 *
 * A page must be observed once to arm it and then stay cold for
 * CXL_CLOCK_STABLE_COLD_EPOCHS further observations, so a window has to be
 * re-walked that many times plus one for any page in it to become demotable.
 */
#define CXL_SAMPLE_SET_PAGES 128
#define CXL_SAMPLE_PASSES    (CXL_CLOCK_STABLE_COLD_EPOCHS + 1)

/*
 * Aging only clears accessed bits, but it still touches shared page-table
 * state and therefore needs its own distributed TLB transaction.
 * Demotion is much more expensive because it performs distributed shootdowns.
 * Keep the two clocks separate so eight per-machine workers cannot multiply
 * either rate.
 */
#define CXL_CLOCK_SCAN_INTERVAL_NS       100000000ULL /* 100 ms */
#define CXL_CLOCK_EPOCH_NS               100000000ULL /* 100 ms */
#define CXL_DEMOTE_INTERVAL_NS           100000000ULL /* 100 ms */
#define CXL_PROMOTION_COOLDOWN_NS        250000000ULL /* 250 ms */
#define CXL_CLOCK_STABLE_COLD_EPOCHS     2
#define CXL_DEMOTE_RATE_BATCH            4

/* Avoid flooding the console while keeping CLOCK decisions observable. */
#define CXL_CLOCK_REPORT_INTERVAL 16384
/* Admission statistics are reported by the background worker, not faults. */
#define CXL_ADMISSION_REPORT_INTERVAL (1UL << 20)

/* Reclaim protocol stats are printed on the first transaction and sparsely. */
#define CXL_PROTOCOL_REPORT_INTERVAL 64
#define CXL_AGING_REPORT_INTERVAL 1024

#ifdef DSM_CXL_DEMOTE_CLOCK
#define CXL_RECLAIM_POLICY_NAME "clock-second-chance"
#else
#define CXL_RECLAIM_POLICY_NAME "fifo"
#endif

struct cxl_saved_mapping {
    pte_t *pte;
    u64 old_pteval;
    mid_t machine_id;
    struct vmspace *vmspace;
    vaddr_t va;
};

/* PTEs changed by aging never become migration entries. */
struct cxl_aged_mapping {
    pte_t *pte;
    mid_t machine_id;
    struct vmspace *vmspace;
    vaddr_t va;
};

struct cxl_alias {
    struct vmspace *vmspace;
    vaddr_t va;
    u64 perm;
    bool read_locked;
};

struct cxl_demote_candidate {
    struct page *page;
    struct pmobject *pmo;
    paddr_t cxl_pa;
    paddr_t origin_pa;
    u64 pmo_index;
    mid_t owner_mid;
    u32 alias_count;
    u32 mapping_count;
    u32 aged_mapping_count;
    u8 policy_perm;
    u64 mapping_generation;
    bool prepared;
    bool pmo_pinned;
    bool referenced;
    struct cxl_alias aliases[CXL_DEMOTE_MAX_ALIASES_PER_PAGE];
    struct cxl_saved_mapping mappings[CXL_DEMOTE_MAX_MAPPINGS_PER_PAGE];
    struct cxl_aged_mapping aged_mappings[
            CXL_DEMOTE_MAX_MAPPINGS_PER_PAGE];
};

static struct cxl_demote_candidate candidates[CXL_DEMOTE_MAX_BATCH];
/* Re-entrancy guard for the per-machine background reclaim syscall. */
static bool cxl_reclaim_on_cpu[PLAT_CPU_NUM];
/* Fault CPUs update only their local slot; the background worker publishes. */
static u64 local_fault_fallbacks[PLAT_CPU_NUM];
static u64 local_fault_fallbacks_published;

extern void remove_page_from_pmo(struct pmobject *pmo, u64 index);
extern void handle_ipi(void);

#ifdef DSM_CXL_DEMOTE_ENABLED
static void cxl_object_get(void *opaque)
{
    struct object *object = container_of(opaque, struct object, opaque);

    atomic_fetch_add_64(&object->refcount, 1);
}
#endif

/* Never resurrect an object after obj_put() has started its destructor. */
static bool cxl_object_try_get(void *opaque)
{
    struct object *object = container_of(opaque, struct object, opaque);
    u64 refcount = __atomic_load_n(&object->refcount, __ATOMIC_ACQUIRE);

    while (refcount != 0) {
        if (__atomic_compare_exchange_n(&object->refcount,
                                        &refcount,
                                        refcount + 1,
                                        true,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return true;
    }
    return false;
}

static void cxl_pmo_mapping_init(struct pmobject *pmo)
{
    u32 expected;

    if (__atomic_load_n(&pmo->cxl_mapping_init_state, __ATOMIC_ACQUIRE)
        == CXL_PMO_MAPPING_READY)
        return;

    expected = CXL_PMO_MAPPING_UNINITIALIZED;
    if (__atomic_compare_exchange_n(&pmo->cxl_mapping_init_state,
                                    &expected,
                                    CXL_PMO_MAPPING_INITIALIZING,
                                    false,
                                    __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
        lock_init(&pmo->cxl_mapping_lock);
        init_list_head(&pmo->cxl_mapping_list);
        __atomic_store_n(&pmo->cxl_mapping_generation,
                         1,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&pmo->cxl_mapping_init_state,
                         CXL_PMO_MAPPING_READY,
                         __ATOMIC_RELEASE);
        return;
    }

    while (__atomic_load_n(&pmo->cxl_mapping_init_state, __ATOMIC_ACQUIRE)
           != CXL_PMO_MAPPING_READY)
        CPU_PAUSE();
}

void dsm_cxl_vmr_init(struct vmregion *vmr)
{
    init_empty_node(&vmr->cxl_pmo_node);
    vmr->cxl_pmo_linked = false;
}

void dsm_cxl_link_vmr(struct vmregion *vmr)
{
#ifndef DSM_CXL_DEMOTE_ENABLED
    UNUSED(vmr);
#else
    struct pmobject *pmo;

    if (!vmr || !vmr->pmo || !is_radix_pmo(vmr->pmo)
        || vmr->cxl_pmo_linked)
        return;
    pmo = vmr->pmo;
    cxl_pmo_mapping_init(pmo);

    lock(&pmo->cxl_mapping_lock);
    if (!vmr->cxl_pmo_linked) {
        cxl_object_get(pmo);
        list_add(&vmr->cxl_pmo_node, &pmo->cxl_mapping_list);
        vmr->cxl_pmo_linked = true;
        __atomic_add_fetch(&pmo->cxl_mapping_generation,
                           1,
                           __ATOMIC_RELEASE);
    }
    unlock(&pmo->cxl_mapping_lock);
#endif
}

void dsm_cxl_unlink_vmr(struct vmregion *vmr)
{
    struct pmobject *pmo;
    bool put_pmo = false;

    if (!vmr || !vmr->pmo || !vmr->cxl_pmo_linked)
        return;
    pmo = vmr->pmo;
    cxl_pmo_mapping_init(pmo);

    lock(&pmo->cxl_mapping_lock);
    if (vmr->cxl_pmo_linked) {
        list_del(&vmr->cxl_pmo_node);
        vmr->cxl_pmo_linked = false;
        __atomic_add_fetch(&pmo->cxl_mapping_generation,
                           1,
                           __ATOMIC_RELEASE);
        put_pmo = true;
    }
    unlock(&pmo->cxl_mapping_lock);

    if (put_pmo)
        obj_put(pmo);
}

#ifdef DSM_CXL_DEMOTE_CLOCK
static void report_clock_stats(u64 events, u64 added)
{
    u64 previous = events - added;

    if (events != added
        && events / CXL_CLOCK_REPORT_INTERVAL
                   == previous / CXL_CLOCK_REPORT_INTERVAL)
        return;

    kinfo("[CXL_SAMPLE] window_pages=%d passes=%d rotations=%lu "
          "cur_pass=%lu cur_step=%lu\n",
          CXL_SAMPLE_SET_PAGES,
          CXL_SAMPLE_PASSES,
          dsm_meta->cxl_reclaim.sample_rotations,
          dsm_meta->cxl_reclaim.sample_passes,
          dsm_meta->cxl_reclaim.sample_steps);
    kinfo("[CXL_CLOCK] scans=%lu second_chances=%lu armed=%lu hot=%lu "
          "one_epoch_cold=%lu "
          "stable_cold=%lu "
          "cold_evictions=%lu pressure_evictions=%lu cooldown_skips=%lu "
          "scan_skips=%lu\n",
          __atomic_load_n(&dsm_meta->cxl_reclaim.clock_scans,
                          __ATOMIC_RELAXED),
          __atomic_load_n(&dsm_meta->cxl_reclaim.clock_second_chances,
                          __ATOMIC_RELAXED),
          __atomic_load_n(&dsm_meta->cxl_reclaim.clock_armed_pages,
                          __ATOMIC_RELAXED),
          __atomic_load_n(&dsm_meta->cxl_reclaim.clock_rereferenced,
                          __ATOMIC_RELAXED),
          __atomic_load_n(&dsm_meta->cxl_reclaim.clock_one_epoch_cold,
                          __ATOMIC_RELAXED),
          __atomic_load_n(&dsm_meta->cxl_reclaim.clock_stable_cold,
                          __ATOMIC_RELAXED),
          __atomic_load_n(&dsm_meta->cxl_reclaim.clock_cold_evictions,
                          __ATOMIC_RELAXED),
          __atomic_load_n(&dsm_meta->cxl_reclaim.clock_pressure_evictions,
                          __ATOMIC_RELAXED),
          __atomic_load_n(&dsm_meta->cxl_reclaim.clock_cooldown_skips,
                          __ATOMIC_RELAXED),
          __atomic_load_n(&dsm_meta->cxl_reclaim.clock_scan_skips,
                          __ATOMIC_RELAXED));
}
#endif

static paddr_t cxl_page_pa(struct page *page)
{
    return virt_to_phys((void *)page_to_virt(page));
}

static struct page *validated_cxl_page(paddr_t pa)
{
    struct page *page;

    if (!IS_SHM_PADDR(pa))
        return NULL;
    page = virt_to_page((void *)phys_to_virt(pa));
    if (!page || page->pool->type != CXL_MEM_PAGE || page->order != 0
        || !page_check_flag(page, PG_allocated) || cxl_page_pa(page) != pa)
        return NULL;
    return page;
}

/*
 * Keep the safety contract narrower than the migration mechanism.  New PMO
 * kinds must be opted in here after their ownership and writeback semantics
 * have been reviewed; radix fallback alone is never sufficient eligibility.
 */
static bool cxl_policy_allows_pmo(struct pmobject *pmo)
{
    return pmo
           && cxl_policy_mapping_eligible(pmo->type,
                                          is_radix_pmo(pmo),
                                          0);
}

static bool cxl_policy_allows_mapping(struct pmobject *pmo, u64 perm)
{
    return pmo
           && cxl_policy_mapping_eligible(pmo->type,
                                          is_radix_pmo(pmo),
                                          perm);
}

/* Mapping-list lifetime is protected by cxl_mapping_lock. */
static bool snapshot_pmo_page_policy(struct pmobject *pmo, u64 pmo_index,
                                     u8 *perm_out, u64 *generation_out)
{
    struct vmregion *vmr;
    u8 perm = 0;
    bool found = false;
    bool eligible = cxl_policy_allows_pmo(pmo);

    lock(&pmo->cxl_mapping_lock);
    *generation_out = pmo->cxl_mapping_generation;
    for_each_in_list (vmr,
                      struct vmregion,
                      cxl_pmo_node,
                      &pmo->cxl_mapping_list) {
        u64 page_count;

        if (!vmr->cxl_pmo_linked || vmr->pmo != pmo || !vmr->vmspace)
            continue;
        page_count = DIV_ROUND_UP(vmr->size, PAGE_SIZE);
        if (pmo_index >= page_count)
            continue;
        found = true;
        perm |= vmr->perm;
        if (!cxl_policy_allows_mapping(pmo, vmr->perm))
            eligible = false;
    }
    unlock(&pmo->cxl_mapping_lock);
    *perm_out = perm;
    return found && eligible;
}

/* fifo_guard serializes current eligible-page accounting with page teardown. */
static void account_page_policy_locked(struct page *page,
                                       struct pmobject *pmo,
                                       bool eligible, u8 perm)
{
    u8 old_perm = page->cxl_policy_perm & (VMR_READ | VMR_WRITE | VMR_EXEC);
    u8 new_perm = perm & (VMR_READ | VMR_WRITE | VMR_EXEC);

    if (page->cxl_policy_eligible
        && (!eligible || old_perm != new_perm)) {
        BUG_ON(pmo->type >= PMO_TYPE_NR);
        BUG_ON(dsm_meta->cxl_reclaim.eligible_by_pmo_type[pmo->type] == 0);
        BUG_ON(dsm_meta->cxl_reclaim.eligible_by_perm[old_perm] == 0);
        dsm_meta->cxl_reclaim.eligible_by_pmo_type[pmo->type]--;
        dsm_meta->cxl_reclaim.eligible_by_perm[old_perm]--;
        page->cxl_policy_eligible = 0;
    }
    if (eligible && !page->cxl_policy_eligible) {
        BUG_ON(pmo->type >= PMO_TYPE_NR);
        dsm_meta->cxl_reclaim.eligible_by_pmo_type[pmo->type]++;
        dsm_meta->cxl_reclaim.eligible_by_perm[new_perm]++;
        page->cxl_policy_eligible = 1;
    }
    page->cxl_policy_perm = new_perm;
}

static void unaccount_page_policy_locked(struct page *page,
                                         struct pmobject *pmo)
{
    if (!page->cxl_policy_eligible || !pmo)
        return;
    BUG_ON(pmo->type >= PMO_TYPE_NR);
    BUG_ON(dsm_meta->cxl_reclaim.eligible_by_pmo_type[pmo->type] == 0);
    BUG_ON(dsm_meta->cxl_reclaim.eligible_by_perm[
                   page->cxl_policy_perm] == 0);
    dsm_meta->cxl_reclaim.eligible_by_pmo_type[pmo->type]--;
    dsm_meta->cxl_reclaim.eligible_by_perm[page->cxl_policy_perm]--;
    page->cxl_policy_eligible = 0;
}

/* fifo_guard protects both the cached policy inputs and aging state. */
static void reset_page_aging_locked(struct page *page, u64 generation)
{
    page->cxl_age_armed = 0;
    page->cxl_cold_epochs = 0;
    page->cxl_age_epoch = 0;
    page->cxl_age_started_ns = 0;
    page->cxl_policy_generation = generation;
}

void dsm_cxl_note_vmr_perm_change(struct vmregion *vmr)
{
    struct pmobject *pmo;

    if (!vmr || !vmr->pmo || !vmr->cxl_pmo_linked)
        return;
    pmo = vmr->pmo;
    lock(&pmo->cxl_mapping_lock);
    __atomic_add_fetch(&pmo->cxl_mapping_generation,
                       1,
                       __ATOMIC_RELEASE);
    unlock(&pmo->cxl_mapping_lock);
}

static inline void set_cxl_reclaim_state(struct page *page, u8 state)
{
    __atomic_store_n(&page->cxl_reclaim_state, state, __ATOMIC_RELEASE);
}

bool dsm_cxl_reclaim_enabled(void)
{
#ifdef DSM_CXL_DEMOTE_ENABLED
    return dsm_meta
           && __atomic_load_n(&dsm_meta->cxl_reclaim.initialized,
                              __ATOMIC_ACQUIRE);
#else
    return false;
#endif
}

bool dsm_cxl_mapping_in_transition(struct pmobject *pmo, u64 pmo_index,
                                   paddr_t pa)
{
    struct page *page;
    u8 state;

    if (!dsm_meta || !pmo
        || !__atomic_load_n(&dsm_meta->cxl_reclaim.initialized,
                            __ATOMIC_ACQUIRE))
        return false;

    /*
     * Only a CXL-resident page can be selected by this demoter.  In
     * particular, the DRAM-to-CXL fault path reaches this predicate before it
     * classifies the PMO page's owner.  Making remote DRAM faults contend on
     * the shared FIFO lock needlessly turns an active demotion into a failed
     * promotion attempt.
     */
    if (!IS_SHM_PADDR(pa))
        return false;

    /*
     * pa was read from the PMO before the caller took its page-table lock.
     * The demoter takes that same lock before snapshotting or changing this
     * mapping.  It therefore cannot both select this page and miss a mapping
     * installed after this check: if selection wins, the release-published
     * page state below is visible; if this fault wins, prepare_candidate()
     * observes the new PTE.  A page-specific state check is sufficient and,
     * unlike try-locking fifo_guard, never delays an unrelated fault merely
     * because CLOCK is scanning another page.
     */
    page = validated_cxl_page(pa);
    if (!page)
        return get_page_from_pmo(pmo, pmo_index) != pa;
    state = __atomic_load_n(&page->cxl_reclaim_state, __ATOMIC_ACQUIRE);
    /*
     * Most ordinary shared-memory pages (including the initial program
     * image) are not DRAM-origin pages tracked by this reclaimer.  A PMO
     * snapshot mismatch for such a page cannot be a demotion and must not
     * turn into an unbounded scheduled refault.
     */
    if (state == CXL_RECLAIM_NONE)
        return false;
    if (state != CXL_RECLAIM_RESIDENT)
        return true;
    return get_page_from_pmo(pmo, pmo_index) != pa;
}

void dsm_cxl_reclaim_init(void)
{
    u64 total_pages = 0;
    u64 limit_pages;
    int i;

    if (CUR_MACHINE_ID != 0)
        return;

#ifndef DSM_CXL_DEMOTE_ENABLED
    __atomic_store_n(&dsm_meta->cxl_reclaim.initialized,
                     0,
                     __ATOMIC_RELEASE);
    kinfo("[CXL_RECLAIM] disabled\n");
    return;
#endif

    for (i = 0; i < cxlmem_map_num; i++)
        total_pages += global_cxl_mem[i]->pool_phys_page_num;
    limit_pages = (u64)DSM_CXL_DEMOTE_LIMIT_MB * 1024 * 1024 / PAGE_SIZE;
    limit_pages = MIN(limit_pages, total_pages);

    lock_init(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    lock_init(&dsm_meta->cxl_reclaim.account_guard.lock);
    lock_init(&dsm_meta->msi_rpc_lock);
    lock_init(&dsm_meta->cxl_control_rpc_lock);
    for (i = 0; i < CLUSTER_MAX_MACHINE_NUM; i++) {
        lock_init(&dsm_meta->cxl_control[i].lock);
        dsm_meta->cxl_control[i].pending = 0;
        dsm_meta->cxl_control[i].sender = 0xFFFFFFFF;
        dsm_meta->cxl_control[i].kind = CXL_CONTROL_TXN_DEMOTE;
        dsm_meta->cxl_control[i].result = 0;
        dsm_meta->cxl_control[i].rpc_id = 0;
        dsm_meta->cxl_control[i].reply_rpc_id = 0;
    }
    init_list_head(&dsm_meta->cxl_reclaim.fifo);
    dsm_meta->cxl_reclaim.total_pages = total_pages;
    dsm_meta->cxl_reclaim.limit_pages = limit_pages;
    dsm_meta->cxl_reclaim.allocated_pages = 0;
    dsm_meta->cxl_reclaim.reserved_pages = 0;
    dsm_meta->cxl_reclaim.resident_pages = 0;
    dsm_meta->cxl_reclaim.resident_reserved_pages = 0;
    dsm_meta->cxl_reclaim.reclaimed_pages = 0;
    dsm_meta->cxl_reclaim.soft_limit_overcommits = 0;
    dsm_meta->cxl_reclaim.async_reclaim_requests = 0;
    dsm_meta->cxl_reclaim.fault_fallbacks = 0;
    dsm_meta->cxl_reclaim.admission_reported_misses = 0;
    dsm_meta->cxl_reclaim.admission_reported_fallbacks = 0;
    dsm_meta->cxl_reclaim.clock_scans = 0;
    dsm_meta->cxl_reclaim.clock_second_chances = 0;
    dsm_meta->cxl_reclaim.clock_cold_evictions = 0;
    dsm_meta->cxl_reclaim.clock_pressure_evictions = 0;
    dsm_meta->cxl_reclaim.clock_scan_skips = 0;
    dsm_meta->cxl_reclaim.clock_cooldown_skips = 0;
    dsm_meta->cxl_reclaim.clock_stable_cold = 0;
    dsm_meta->cxl_reclaim.clock_armed_pages = 0;
    dsm_meta->cxl_reclaim.clock_rereferenced = 0;
    dsm_meta->cxl_reclaim.clock_one_epoch_cold = 0;
    dsm_meta->cxl_reclaim.aging_flush_batches = 0;
    dsm_meta->cxl_reclaim.aging_flush_pages = 0;
    dsm_meta->cxl_reclaim.aging_flush_machines = 0;
    dsm_meta->cxl_reclaim.aging_flush_latency_ns = 0;
    dsm_meta->cxl_reclaim.aging_flush_max_ns = 0;
    dsm_meta->cxl_reclaim.next_age_epoch = 1;
    memset((void *)dsm_meta->cxl_reclaim.eligible_by_pmo_type,
           0,
           sizeof(dsm_meta->cxl_reclaim.eligible_by_pmo_type));
    memset((void *)dsm_meta->cxl_reclaim.eligible_by_perm,
           0,
           sizeof(dsm_meta->cxl_reclaim.eligible_by_perm));
    dsm_meta->cxl_reclaim.promotion_pages = 0;
    dsm_meta->cxl_reclaim.refault_pages = 0;
    dsm_meta->cxl_reclaim.demote_conflicts = 0;
    dsm_meta->cxl_reclaim.demote_transactions = 0;
    dsm_meta->cxl_reclaim.demote_phase_prepare_ns = 0;
    dsm_meta->cxl_reclaim.demote_phase_flush_ns = 0;
    dsm_meta->cxl_reclaim.demote_phase_copy_ns = 0;
    dsm_meta->cxl_reclaim.demote_phase_bitmap_ns = 0;
    dsm_meta->cxl_reclaim.demote_phase_finish_ns = 0;
    dsm_meta->cxl_reclaim.demote_phase_total_ns = 0;
    dsm_meta->cxl_reclaim.demote_phase_max_ns = 0;
    dsm_meta->cxl_reclaim.next_scan_ns = 0;
    dsm_meta->cxl_reclaim.next_demote_ns = 0;
    dsm_meta->cxl_reclaim.sample_cursor = NULL;
    dsm_meta->cxl_reclaim.sample_start = NULL;
    dsm_meta->cxl_reclaim.sample_steps = 0;
    dsm_meta->cxl_reclaim.sample_passes = 0;
    dsm_meta->cxl_reclaim.sample_rotations = 0;
    dsm_meta->cxl_reclaim.next_pressure_ns = 0;
    memset(dsm_meta->cxl_reclaim.recent_demote_pa,
           0,
           sizeof(dsm_meta->cxl_reclaim.recent_demote_pa));
    dsm_meta->cxl_reclaim.free_pending_pages = 0;
    dsm_meta->cxl_reclaim.next_sequence = 1;
    dsm_meta->cxl_reclaim.next_rpc_id = 1;
    dsm_meta->cxl_reclaim.pending_reclaim_pages = 0;
    __atomic_store_n(&dsm_meta->cxl_reclaim.reclaiming,
                     0,
                     __ATOMIC_RELEASE);
    dsm_meta->cxl_reclaim.snapshotting = 0;
    __atomic_store_n(&dsm_meta->cxl_reclaim.initialized,
                     1,
                     __ATOMIC_RELEASE);

    kinfo("[CXL_RECLAIM] initialized policy=%s "
          "total_pages=%lu limit_pages=%lu limit_mb=%d async_batch=%d "
          "scan_limit=%d scan_interval_ns=%lu demote_interval_ns=%lu "
          "epoch_ns=%lu rate_batch=%d\n",
          CXL_RECLAIM_POLICY_NAME,
          total_pages,
          limit_pages,
          DSM_CXL_DEMOTE_LIMIT_MB,
          CXL_DEMOTE_MAX_BATCH,
          CXL_DEMOTE_SELECT_SCAN_LIMIT,
          CXL_CLOCK_SCAN_INTERVAL_NS,
          CXL_DEMOTE_INTERVAL_NS,
          CXL_CLOCK_EPOCH_NS,
          CXL_DEMOTE_RATE_BATCH);
}

int dsm_cxl_reserve_pages(u64 pages)
{
    u64 projected;

    if (!dsm_meta
        || !__atomic_load_n(&dsm_meta->cxl_reclaim.initialized,
                            __ATOMIC_ACQUIRE))
        return 0;

    lock(&dsm_meta->cxl_reclaim.account_guard.lock);
    projected = dsm_meta->cxl_reclaim.allocated_pages
                + dsm_meta->cxl_reclaim.reserved_pages + pages;
    if (projected < dsm_meta->cxl_reclaim.allocated_pages
        || projected > dsm_meta->cxl_reclaim.total_pages) {
        unlock(&dsm_meta->cxl_reclaim.account_guard.lock);
        return -ENOMEM;
    }
    dsm_meta->cxl_reclaim.reserved_pages += pages;
    unlock(&dsm_meta->cxl_reclaim.account_guard.lock);
    return 0;
}

void dsm_cxl_commit_reserved_pages(u64 pages)
{
    if (!dsm_meta
        || !__atomic_load_n(&dsm_meta->cxl_reclaim.initialized,
                            __ATOMIC_ACQUIRE))
        return;

    lock(&dsm_meta->cxl_reclaim.account_guard.lock);
    BUG_ON(dsm_meta->cxl_reclaim.reserved_pages < pages);
    dsm_meta->cxl_reclaim.reserved_pages -= pages;
    dsm_meta->cxl_reclaim.allocated_pages += pages;
    unlock(&dsm_meta->cxl_reclaim.account_guard.lock);
}

void dsm_cxl_cancel_reserved_pages(u64 pages)
{
    if (!dsm_meta
        || !__atomic_load_n(&dsm_meta->cxl_reclaim.initialized,
                            __ATOMIC_ACQUIRE))
        return;

    lock(&dsm_meta->cxl_reclaim.account_guard.lock);
    BUG_ON(dsm_meta->cxl_reclaim.reserved_pages < pages);
    dsm_meta->cxl_reclaim.reserved_pages -= pages;
    unlock(&dsm_meta->cxl_reclaim.account_guard.lock);
}

int dsm_cxl_reserve_resident_pages(u64 pages)
{
    u64 resident;
    u64 reserved;
    u64 limit;
    bool over_limit;

    if (!dsm_meta
        || !__atomic_load_n(&dsm_meta->cxl_reclaim.initialized,
                            __ATOMIC_ACQUIRE))
        return 0;
    if (pages == 0)
        return -EINVAL;

    /*
     * account_guard covers counters only; the potentially blocking FIFO scan
     * and distributed demotion transaction use a different lock.  Crossing
     * the threshold is therefore an accounting operation, never a wait for
     * reclaim.
     */
    lock(&dsm_meta->cxl_reclaim.account_guard.lock);
    resident = dsm_meta->cxl_reclaim.resident_pages;
    reserved = dsm_meta->cxl_reclaim.resident_reserved_pages;
    limit = dsm_meta->cxl_reclaim.limit_pages;
    if (reserved + pages < reserved) {
        unlock(&dsm_meta->cxl_reclaim.account_guard.lock);
        return -ENOMEM;
    }
    reserved += pages;
    dsm_meta->cxl_reclaim.resident_reserved_pages = reserved;
    over_limit = resident > limit || reserved > limit - MIN(resident, limit);
    if (over_limit) {
        /*
         * These counters share the accounting critical section already
         * required by the reservation.  Avoid an additional cross-machine
         * atomic operation on every over-limit fault/read-ahead page.
         */
        dsm_meta->cxl_reclaim.soft_limit_overcommits += pages;
        if (dsm_meta->cxl_reclaim.pending_reclaim_pages < pages) {
            dsm_meta->cxl_reclaim.pending_reclaim_pages = pages;
            dsm_meta->cxl_reclaim.async_reclaim_requests++;
        }
    }
    unlock(&dsm_meta->cxl_reclaim.account_guard.lock);
    return 0;
}

void dsm_cxl_cancel_resident_pages(u64 pages)
{
    if (!dsm_meta
        || !__atomic_load_n(&dsm_meta->cxl_reclaim.initialized,
                            __ATOMIC_ACQUIRE))
        return;

    lock(&dsm_meta->cxl_reclaim.account_guard.lock);
    BUG_ON(dsm_meta->cxl_reclaim.resident_reserved_pages < pages);
    dsm_meta->cxl_reclaim.resident_reserved_pages -= pages;
    unlock(&dsm_meta->cxl_reclaim.account_guard.lock);
}

void dsm_cxl_note_fault_fallback(void)
{
    if (!dsm_meta
        || !__atomic_load_n(&dsm_meta->cxl_reclaim.initialized,
                            __ATOMIC_ACQUIRE))
        return;
    __atomic_add_fetch(&local_fault_fallbacks[smp_get_cpu_id()],
                       1,
                       __ATOMIC_RELAXED);
}

int dsm_cxl_snapshot_begin(void)
{
    u64 deadline;

    if (!dsm_cxl_reclaim_enabled())
        return 0;
    deadline = plat_get_mono_time() + DSM_CXL_RPC_TIMEOUT_NS;
    while (plat_get_mono_time() < deadline) {
        lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
        if (!dsm_meta->cxl_reclaim.reclaiming) {
            dsm_meta->cxl_reclaim.snapshotting++;
            unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
            return 0;
        }
        unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
        handle_ipi();
        CPU_PAUSE();
    }
    return -ETIMEDOUT;
}

void dsm_cxl_snapshot_end(void)
{
    if (!dsm_cxl_reclaim_enabled())
        return;
    lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    BUG_ON(dsm_meta->cxl_reclaim.snapshotting == 0);
    dsm_meta->cxl_reclaim.snapshotting--;
    unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
}

u64 dsm_cxl_reclaimed_pages(void)
{
    u64 reclaimed = 0;

    if (!dsm_meta
        || !__atomic_load_n(&dsm_meta->cxl_reclaim.initialized,
                            __ATOMIC_ACQUIRE))
        return 0;
    lock(&dsm_meta->cxl_reclaim.account_guard.lock);
    reclaimed = dsm_meta->cxl_reclaim.reclaimed_pages;
    unlock(&dsm_meta->cxl_reclaim.account_guard.lock);
    return reclaimed;
}

int dsm_cxl_track_pages(struct cxl_track_op *ops, u64 count)
{
    struct page *pages[CXL_DEMOTE_MAX_BATCH] = { 0 };
    u64 tracked = 0;
    u64 refaulted = 0;
    u64 now;
    u64 i;

    if (!dsm_cxl_reclaim_enabled())
        return -EOPNOTSUPP;
    if (!dsm_meta || !ops || count == 0 || count > CXL_DEMOTE_MAX_BATCH)
        return -EINVAL;

    /* Validate PMO ownership without extending the shared FIFO critical path. */
    for (i = 0; i < count; i++) {
        struct cxl_track_op *op = &ops[i];

        op->result = 0;
        if (!op->pmo || !op->vmspace || op->owner_mid < 0
            || op->owner_mid >= CLUSTER_MACHINE_NUM) {
            op->result = -EINVAL;
            continue;
        }
        pages[i] = validated_cxl_page(op->cxl_pa);
        if (!pages[i]
            || get_paddr_machine_id(op->origin_pa) != op->owner_mid
            || get_page_from_pmo(op->pmo, op->pmo_index) != op->cxl_pa)
            op->result = -EINVAL;
    }

    /*
     * cxl_promoted_ns is written by whichever machine took the fault and read
     * by whichever machine wins the reclaim token, so it must be on the
     * cluster clock -- plat_get_mono_time() counts from the local boot.
     */
    now = dsm_cluster_time_ns();

    /* Publish the whole promotion batch with one FIFO/accounting lock pair. */
    lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    for (i = 0; i < count; i++) {
        struct cxl_track_op *op = &ops[i];
        struct page *page = pages[i];
        u8 policy_perm;
        bool policy_eligible;

        if (op->result)
            continue;
        if (page->cxl_reclaim_state != CXL_RECLAIM_NONE) {
            op->result = -EEXIST;
            continue;
        }

        init_empty_node(&page->cxl_reclaim_node);
        page->cxl_origin_pa = op->origin_pa;
        page->cxl_pmo = (u64)op->pmo;
        page->cxl_pmo_index = op->pmo_index;
        page->cxl_owner_mid = op->owner_mid;
        page->cxl_sequence = dsm_meta->cxl_reclaim.next_sequence++;
        page->cxl_reclaim_phase = CXL_RECLAIM_PHASE_IDLE;
        page->cxl_free_requested = 0;
        page->cxl_origin_release_state = CXL_ORIGIN_NOT_RELEASED;
        page->cxl_scanning = 0;
        page->cxl_cold_epochs = 0;
        page->cxl_speculative = op->speculative;
        page->cxl_age_armed = 0;
        page->cxl_age_epoch = 0;
        page->cxl_age_started_ns = 0;
        page->cxl_policy_eligible = 0;
        page->cxl_policy_perm = 0;
        page->cxl_policy_generation = 0;
        policy_eligible = snapshot_pmo_page_policy(op->pmo,
                                                   op->pmo_index,
                                                   &policy_perm,
                                                   &page->cxl_policy_generation);
        account_page_policy_locked(
                page,
                op->pmo,
                policy_eligible,
                policy_perm);
        page->cxl_promoted_ns = now;
        set_cxl_reclaim_state(page, CXL_RECLAIM_RESIDENT);
        list_append(&page->cxl_reclaim_node, &dsm_meta->cxl_reclaim.fifo);
        tracked++;
    }

    lock(&dsm_meta->cxl_reclaim.account_guard.lock);
    BUG_ON(dsm_meta->cxl_reclaim.resident_reserved_pages < tracked);
    dsm_meta->cxl_reclaim.resident_reserved_pages -= tracked;
    BUG_ON(dsm_meta->cxl_reclaim.resident_pages + tracked
           < dsm_meta->cxl_reclaim.resident_pages);
    dsm_meta->cxl_reclaim.resident_pages += tracked;
    dsm_meta->cxl_reclaim.promotion_pages += tracked;
    for (i = 0; i < count; i++) {
        u64 slot;

        if (ops[i].result)
            continue;
        slot = (ops[i].origin_pa >> PAGE_SHIFT)
               & (CXL_RECENT_DEMOTE_SLOTS - 1);
        if (dsm_meta->cxl_reclaim.recent_demote_pa[slot]
            == ops[i].origin_pa) {
            dsm_meta->cxl_reclaim.recent_demote_pa[slot] = 0;
            refaulted++;
        }
    }
    dsm_meta->cxl_reclaim.refault_pages += refaulted;
    unlock(&dsm_meta->cxl_reclaim.account_guard.lock);
    unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);

    return 0;
}

int dsm_cxl_track_page(paddr_t cxl_pa, paddr_t origin_pa,
                       struct pmobject *pmo, u64 pmo_index,
                       struct vmspace *vmspace, vaddr_t va,
                       u64 perm, mid_t owner_mid)
{
    struct cxl_track_op op = {
        .cxl_pa = cxl_pa,
        .origin_pa = origin_pa,
        .pmo = pmo,
        .pmo_index = pmo_index,
        .vmspace = vmspace,
        .va = va,
        .perm = perm,
        .owner_mid = owner_mid,
        .speculative = false,
    };
    int ret;

    ret = dsm_cxl_track_pages(&op, 1);
    return ret ? ret : op.result;
}

static bool try_pin_candidate_pmo(struct page *page, struct pmobject **pmo_out)
{
    struct pmobject *pmo = (struct pmobject *)page->cxl_pmo;
    bool pinned = false;

    if (!pmo)
        return false;
    /*
     * select_candidates() calls this with the shared FIFO/CLOCK state lock
     * held.  Never wait for a PMO mapping lock from here, and never run the
     * lazy initializer either: an eviction candidate is optional, so skip it
     * and let a later pass pick it up without inverting the lock order.
     */
    if (__atomic_load_n(&pmo->cxl_mapping_init_state, __ATOMIC_ACQUIRE)
        != CXL_PMO_MAPPING_READY)
        return false;
    if (try_lock(&pmo->cxl_mapping_lock) != 0)
        return false;
    if (!list_empty(&pmo->cxl_mapping_list)) {
        pinned = cxl_object_try_get(pmo);
    }
    unlock(&pmo->cxl_mapping_lock);

    if (pinned)
        *pmo_out = pmo;
    return pinned;
}

/*
 * Cursor helpers.  All of these run with fifo_guard held.
 *
 * The cursors are plain pointers into the FIFO, so anything that unlinks an
 * entry has to move them off it first -- see cxl_sample_forget_node().
 */
static inline struct list_head *cxl_sample_step(struct list_head *node)
{
    struct list_head *head = &dsm_meta->cxl_reclaim.fifo;

    node = node->next;
    /* Skip the sentinel rather than handing it out as an entry. */
    return node == head ? head->next : node;
}

static void cxl_sample_reset_window_locked(void)
{
    struct list_head *head = &dsm_meta->cxl_reclaim.fifo;

    dsm_meta->cxl_reclaim.sample_start = list_empty(head) ? NULL : head->next;
    dsm_meta->cxl_reclaim.sample_cursor = dsm_meta->cxl_reclaim.sample_start;
    dsm_meta->cxl_reclaim.sample_steps = 0;
    dsm_meta->cxl_reclaim.sample_passes = 0;
}

/*
 * Keep the cursors off an entry that is about to leave the FIFO.  Moving to
 * the successor preserves scan order; if the list empties, the window is
 * rebuilt on the next selection.
 */
static void cxl_sample_forget_node(struct list_head *node)
{
    struct list_head *head = &dsm_meta->cxl_reclaim.fifo;
    struct list_head *next = node->next == head ? NULL : node->next;

    if (dsm_meta->cxl_reclaim.sample_cursor == node)
        dsm_meta->cxl_reclaim.sample_cursor = next;
    if (dsm_meta->cxl_reclaim.sample_start == node) {
        /*
         * The window lost its anchor, so the current pass can no longer be
         * compared against it.  Start a fresh window from the successor.
         */
        dsm_meta->cxl_reclaim.sample_start = next;
        dsm_meta->cxl_reclaim.sample_steps = 0;
        dsm_meta->cxl_reclaim.sample_passes = 0;
    }
}

/*
 * Advance the window bookkeeping after one entry has been inspected.  A pass
 * ends after CXL_SAMPLE_SET_PAGES steps: re-walk the same window until it has
 * been seen CXL_SAMPLE_PASSES times, then adopt the following window, which
 * the cursor is already sitting on.
 */
static void cxl_sample_note_step_locked(void)
{
    if (++dsm_meta->cxl_reclaim.sample_steps < CXL_SAMPLE_SET_PAGES)
        return;
    dsm_meta->cxl_reclaim.sample_steps = 0;
    if (++dsm_meta->cxl_reclaim.sample_passes < CXL_SAMPLE_PASSES) {
        dsm_meta->cxl_reclaim.sample_cursor =
                dsm_meta->cxl_reclaim.sample_start;
        return;
    }
    dsm_meta->cxl_reclaim.sample_passes = 0;
    dsm_meta->cxl_reclaim.sample_start = dsm_meta->cxl_reclaim.sample_cursor;
    dsm_meta->cxl_reclaim.sample_rotations++;
}

static u32 select_candidates(u32 limit)
{
    struct list_head *head = &dsm_meta->cxl_reclaim.fifo;
    struct list_head *node;
    struct list_head *last;
    u32 count = 0;
    u32 scanned = 0;
    u32 skipped = 0;

    lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    if (list_empty(head)) {
        unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
        return 0;
    }
#ifdef DSM_CXL_DEMOTE_CLOCK
    /*
     * The hand is a cursor over a bounded window of the FIFO rather than the
     * queue head, and inspected entries are left where they are.  Advancing a
     * cursor cannot be pinned by a contended page any more than rotating it
     * could, and not writing to two neighbours per inspection keeps this loop
     * off the shared cache lines of a multi-million-entry list.
     */
    if (!dsm_meta->cxl_reclaim.sample_cursor
        || !dsm_meta->cxl_reclaim.sample_start)
        cxl_sample_reset_window_locked();
    node = dsm_meta->cxl_reclaim.sample_cursor;
    last = NULL;
#else
    node = head->next;
    last = NULL;
#endif
    while (node && node != head && count < limit
           && scanned < CXL_DEMOTE_SELECT_SCAN_LIMIT) {
        struct page *page =
                list_entry(node, struct page, cxl_reclaim_node);
        struct pmobject *pmo = NULL;
        bool final_entry = node == last;

#ifdef DSM_CXL_DEMOTE_CLOCK
        node = cxl_sample_step(node);
        dsm_meta->cxl_reclaim.sample_cursor = node;
        cxl_sample_note_step_locked();
        /* The window may have rewound, so re-read where to continue from. */
        node = dsm_meta->cxl_reclaim.sample_cursor;
#else
        node = node->next;
#endif
        scanned++;
        if (page->cxl_reclaim_state != CXL_RECLAIM_RESIDENT
            || page->cxl_scanning
            || !try_pin_candidate_pmo(page, &pmo)) {
            skipped++;
            if (final_entry)
                break;
            continue;
        }
        if (!cxl_policy_allows_pmo(pmo)
            || (!page->cxl_policy_eligible
                && page->cxl_policy_generation
                           == __atomic_load_n(
                                   &pmo->cxl_mapping_generation,
                                   __ATOMIC_ACQUIRE))) {
            obj_put(pmo);
            skipped++;
            if (final_entry)
                break;
            continue;
        }

        /*
         * Scanning pins metadata against destruction without changing the
         * externally visible RESIDENT state.  Faults can keep mapping and
         * accessing this page while the aging transaction runs.
         */
        page->cxl_scanning = 1;
        memset(&candidates[count], 0, sizeof(candidates[count]));
        candidates[count].page = page;
        candidates[count].pmo = pmo;
        candidates[count].pmo_pinned = true;
        candidates[count].cxl_pa = cxl_page_pa(page);
        candidates[count].origin_pa = page->cxl_origin_pa;
        candidates[count].pmo_index = page->cxl_pmo_index;
        candidates[count].owner_mid = page->cxl_owner_mid;
        count++;
        if (final_entry)
            break;
    }
    unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
#ifdef DSM_CXL_DEMOTE_CLOCK
    __atomic_add_fetch(&dsm_meta->cxl_reclaim.clock_scans,
                       scanned,
                       __ATOMIC_RELAXED);
    if (skipped)
        __atomic_add_fetch(&dsm_meta->cxl_reclaim.clock_scan_skips,
                           skipped,
                           __ATOMIC_RELAXED);
#else
    UNUSED(skipped);
#endif
    return count;
}

static void release_candidate_aliases(struct cxl_demote_candidate *candidate)
{
    u32 i;

    for (i = 0; i < candidate->alias_count; i++) {
        struct cxl_alias *alias = &candidate->aliases[i];

        if (!alias->vmspace)
            continue;
        if (alias->read_locked)
            read_unlock(&alias->vmspace->vmspace_lock);
        obj_put(alias->vmspace);
        alias->vmspace = NULL;
        alias->read_locked = false;
    }
    candidate->alias_count = 0;
}

#ifdef DSM_CXL_DEMOTE_CLOCK
static void unlock_candidate_aliases(struct cxl_demote_candidate *candidate)
{
    u32 i;

    for (i = 0; i < candidate->alias_count; i++) {
        struct cxl_alias *alias = &candidate->aliases[i];

        if (alias->vmspace && alias->read_locked) {
            read_unlock(&alias->vmspace->vmspace_lock);
            alias->read_locked = false;
        }
    }
}
#endif

static int snapshot_candidate_aliases(struct cxl_demote_candidate *candidate)
{
    struct pmobject *pmo = candidate->pmo;
    struct vmregion *vmr;
    u32 count = 0;

    lock(&pmo->cxl_mapping_lock);
    candidate->mapping_generation = pmo->cxl_mapping_generation;
    for_each_in_list (vmr,
                      struct vmregion,
                      cxl_pmo_node,
                      &pmo->cxl_mapping_list) {
        u64 page_count;

        if (!vmr->cxl_pmo_linked || vmr->pmo != pmo || !vmr->vmspace)
            continue;
        page_count = DIV_ROUND_UP(vmr->size, PAGE_SIZE);
        if (candidate->pmo_index >= page_count)
            continue;
        if (count == CXL_DEMOTE_MAX_ALIASES_PER_PAGE) {
            kwarn_once(
                    "[CXL_RECLAIM] pmo=%p index=%lu has more than %u aliases; "
                    "page cannot be reclaimed\n",
                    pmo,
                    candidate->pmo_index,
                    CXL_DEMOTE_MAX_ALIASES_PER_PAGE);
            unlock(&pmo->cxl_mapping_lock);
            candidate->alias_count = count;
            release_candidate_aliases(candidate);
            return -E2BIG;
        }
        candidate->aliases[count].vmspace =
                (struct vmspace *)vmr->vmspace;
        candidate->aliases[count].va =
                vmr->start + candidate->pmo_index * PAGE_SIZE;
        candidate->aliases[count].perm = vmr->perm;
        candidate->aliases[count].read_locked = false;
        if (!cxl_object_try_get(candidate->aliases[count].vmspace)) {
            unlock(&pmo->cxl_mapping_lock);
            candidate->alias_count = count;
            release_candidate_aliases(candidate);
            return -EAGAIN;
        }
        count++;
    }
    unlock(&pmo->cxl_mapping_lock);
    candidate->alias_count = count;
    return 0;
}

/*
 * Refresh cached policy inputs without clearing Accessed or changing the
 * externally visible page state.  FIFO uses this before claiming DEMOTING;
 * CLOCK performs the stronger live validation in prepare_candidate_aging().
 */
#ifndef DSM_CXL_DEMOTE_CLOCK
static int refresh_candidate_policy(struct cxl_demote_candidate *candidate)
{
    u32 i;
    u8 perm = 0;
    bool eligible = true;
    int ret;

    ret = snapshot_candidate_aliases(candidate);
    if (ret) {
        if (ret == -E2BIG) {
            lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
            if (candidate->page->cxl_scanning
                && candidate->page->cxl_reclaim_state
                           == CXL_RECLAIM_RESIDENT) {
                account_page_policy_locked(candidate->page,
                                           candidate->pmo,
                                           false,
                                           0);
                reset_page_aging_locked(candidate->page,
                                         candidate->mapping_generation);
            }
            unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
        }
        return ret;
    }
    if (candidate->alias_count == 0)
        eligible = false;
    for (i = 0; i < candidate->alias_count; i++) {
        perm |= candidate->aliases[i].perm;
        if (!cxl_policy_allows_mapping(candidate->pmo,
                                       candidate->aliases[i].perm))
            eligible = false;
    }
    release_candidate_aliases(candidate);

    if (candidate->mapping_generation
        != __atomic_load_n(&candidate->pmo->cxl_mapping_generation,
                           __ATOMIC_ACQUIRE))
        return -EAGAIN;

    lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    if (!candidate->page->cxl_scanning
        || candidate->page->cxl_reclaim_state != CXL_RECLAIM_RESIDENT) {
        unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
        return -EAGAIN;
    }
    account_page_policy_locked(candidate->page,
                               candidate->pmo,
                               eligible,
                               perm);
    candidate->page->cxl_policy_generation = candidate->mapping_generation;
    if (!eligible)
        reset_page_aging_locked(candidate->page,
                                candidate->mapping_generation);
    unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    return eligible ? 0 : -EPERM;
}
#endif

static void restore_candidate_ptes(struct cxl_demote_candidate *candidate)
{
    u32 i;

    for (i = 0; i < candidate->mapping_count; i++) {
        lock(&candidate->mappings[i].vmspace->pgtbl_lock);
        candidate->mappings[i].pte->pteval =
                candidate->mappings[i].old_pteval;
        unlock(&candidate->mappings[i].vmspace->pgtbl_lock);
    }
    candidate->mapping_count = 0;
}

#ifdef DSM_CXL_DEMOTE_CLOCK
/*
 * Clear Accessed under the page-table lock while retaining vmspace/VMR pins.
 * The caller must complete a target-machine TLB shootdown before publishing a
 * new epoch.  Hardware can set Accessed concurrently, hence fetch-and rather
 * than a load followed by a plain PTE store.
 */
static int prepare_candidate_aging(struct cxl_demote_candidate *candidate)
{
    u32 alias_idx;
    int ret;

    candidate->referenced = false;
    candidate->policy_perm = 0;
    candidate->aged_mapping_count = 0;
    ret = snapshot_candidate_aliases(candidate);
    if (ret)
        return ret;
    if (candidate->alias_count == 0)
        return -ENOENT;
    if (!cxl_policy_allows_pmo(candidate->pmo)) {
        release_candidate_aliases(candidate);
        return -EPERM;
    }

    /* Reject an unsafe alias set before changing any PTE. */
    for (alias_idx = 0; alias_idx < candidate->alias_count; alias_idx++) {
        struct cxl_alias *alias = &candidate->aliases[alias_idx];

        if (!cxl_policy_allows_mapping(candidate->pmo, alias->perm)) {
            release_candidate_aliases(candidate);
            return -EPERM;
        }
        candidate->policy_perm |= alias->perm;
    }

    for (alias_idx = 0; alias_idx < candidate->alias_count; alias_idx++) {
        struct cxl_alias *alias = &candidate->aliases[alias_idx];
        struct vmregion *vmr;
        int mid;

        read_lock(&alias->vmspace->vmspace_lock);
        alias->read_locked = true;
        lock(&alias->vmspace->pgtbl_lock);
        vmr = find_vmr_for_va(alias->vmspace, alias->va);
        if (!vmr || vmr->pmo != candidate->pmo
            || (alias->va - vmr->start) / PAGE_SIZE
                       != candidate->pmo_index
            || vmr->perm != alias->perm
            || !cxl_policy_allows_mapping(candidate->pmo, vmr->perm)
            || get_page_from_pmo(candidate->pmo, candidate->pmo_index)
                       != candidate->cxl_pa) {
            unlock(&alias->vmspace->pgtbl_lock);
            ret = -EAGAIN;
            goto out;
        }

        for (mid = 0; mid < CLUSTER_MACHINE_NUM; mid++) {
            void *pgtbl = get_vmspace_pgtbl(alias->vmspace, mid);
            paddr_t pa = 0;
            pte_t *pte = NULL;

            if (!pgtbl)
                continue;
            query_in_pgtbl(pgtbl, alias->va, &pa, &pte);
            if (!pte || !pte->pte_4K.present)
                continue;
            if (is_migration_entry(pte) || pa != candidate->cxl_pa) {
                unlock(&alias->vmspace->pgtbl_lock);
                ret = -EAGAIN;
                goto out;
            }
            if (candidate->aged_mapping_count
                == CXL_DEMOTE_MAX_MAPPINGS_PER_PAGE) {
                unlock(&alias->vmspace->pgtbl_lock);
                ret = -E2BIG;
                goto out;
            }
            candidate->aged_mappings[candidate->aged_mapping_count].pte = pte;
            candidate->aged_mappings[candidate->aged_mapping_count].machine_id =
                    mid;
            candidate->aged_mappings[candidate->aged_mapping_count].vmspace =
                    alias->vmspace;
            candidate->aged_mappings[candidate->aged_mapping_count].va =
                    alias->va;
            candidate->aged_mapping_count++;
            if (__atomic_fetch_and(&pte->pteval,
                                   ~PAGE_ACCESSED,
                                   __ATOMIC_ACQ_REL)
                & PAGE_ACCESSED)
                candidate->referenced = true;
        }
        unlock(&alias->vmspace->pgtbl_lock);
    }
    if (candidate->aged_mapping_count == 0) {
        ret = -ENOENT;
        goto out;
    }

    /* Foreground map/unmap must never wait for a remote aging acknowledgement. */
    unlock_candidate_aliases(candidate);
    return 0;

out:
    /* A failed/incomplete arm is conservatively hot. */
    for (alias_idx = 0; alias_idx < candidate->aged_mapping_count;
         alias_idx++) {
        struct cxl_aged_mapping *mapping =
                &candidate->aged_mappings[alias_idx];
        void *pgtbl = get_vmspace_pgtbl(mapping->vmspace,
                                        mapping->machine_id);
        paddr_t pa = 0;
        pte_t *pte = NULL;

        if (!pgtbl)
            continue;
        lock(&mapping->vmspace->pgtbl_lock);
        query_in_pgtbl(pgtbl, mapping->va, &pa, &pte);
        if (pte && pte->pte_4K.present && !is_migration_entry(pte)
            && pa == candidate->cxl_pa)
            __atomic_fetch_or(&pte->pteval,
                              PAGE_ACCESSED,
                              __ATOMIC_RELEASE);
        unlock(&mapping->vmspace->pgtbl_lock);
    }
    candidate->aged_mapping_count = 0;
    release_candidate_aliases(candidate);
    return ret;
}

static void abort_candidate_aging(struct cxl_demote_candidate *candidate)
{
    u32 i;

    for (i = 0; i < candidate->aged_mapping_count; i++) {
        struct cxl_aged_mapping *mapping = &candidate->aged_mappings[i];
        void *pgtbl = get_vmspace_pgtbl(mapping->vmspace,
                                        mapping->machine_id);
        paddr_t pa = 0;
        pte_t *pte = NULL;

        if (!pgtbl)
            continue;
        lock(&mapping->vmspace->pgtbl_lock);
        query_in_pgtbl(pgtbl, mapping->va, &pa, &pte);
        if (pte && pte->pte_4K.present && !is_migration_entry(pte)
            && pa == candidate->cxl_pa)
            __atomic_fetch_or(&pte->pteval,
                              PAGE_ACCESSED,
                              __ATOMIC_RELEASE);
        unlock(&mapping->vmspace->pgtbl_lock);
    }
    candidate->aged_mapping_count = 0;
    release_candidate_aliases(candidate);
}

static bool validate_candidate_aging(struct cxl_demote_candidate *candidate)
{
    u32 i;
    bool valid = true;

    lock(&candidate->pmo->cxl_mapping_lock);
    if (candidate->mapping_generation
        != candidate->pmo->cxl_mapping_generation)
        valid = false;
    unlock(&candidate->pmo->cxl_mapping_lock);
    if (!valid
        || get_page_from_pmo(candidate->pmo, candidate->pmo_index)
                   != candidate->cxl_pa)
        return false;

    for (i = 0; i < candidate->alias_count; i++) {
        struct cxl_alias *alias = &candidate->aliases[i];
        struct vmregion *vmr;

        read_lock(&alias->vmspace->vmspace_lock);
        vmr = find_vmr_for_va(alias->vmspace, alias->va);
        valid = vmr && vmr->pmo == candidate->pmo
                && (alias->va - vmr->start) / PAGE_SIZE
                           == candidate->pmo_index
                && vmr->perm == alias->perm
                && cxl_policy_allows_mapping(candidate->pmo, vmr->perm);
        read_unlock(&alias->vmspace->vmspace_lock);
        if (!valid)
            return false;
    }
    return true;
}

static enum cxl_page_observation cxl_policy_observe_locked(
        struct cxl_demote_candidate *candidate, u64 epoch, u64 started_ns)
{
    struct page *page = candidate->page;
    struct cxl_policy_page_state state = {
            .armed = page->cxl_age_armed,
            .cold_epochs = page->cxl_cold_epochs,
    };
    enum cxl_page_observation observation;
    bool rebased;

    account_page_policy_locked(page,
                               candidate->pmo,
                               true,
                               candidate->policy_perm);
    rebased = cxl_policy_rebase_mapping_generation(
            &state,
            page->cxl_policy_generation,
            candidate->mapping_generation);
    if (rebased) {
        page->cxl_age_epoch = 0;
        page->cxl_age_started_ns = 0;
    }
    observation = cxl_policy_observe_epoch(
            &state,
            candidate->referenced,
            CXL_CLOCK_STABLE_COLD_EPOCHS);
    page->cxl_age_armed = state.armed;
    page->cxl_cold_epochs = state.cold_epochs;
    page->cxl_policy_generation = candidate->mapping_generation;
    page->cxl_age_epoch = epoch;
    page->cxl_age_started_ns = started_ns;
    return observation;
}
#endif

static int prepare_candidate(struct cxl_demote_candidate *candidate)
{
    u32 alias_idx;
    int ret;

    ret = snapshot_candidate_aliases(candidate);
    if (ret)
        return ret;
    if (candidate->alias_count == 0)
        return -ENOENT;
    if (!cxl_policy_allows_pmo(candidate->pmo)) {
        release_candidate_aliases(candidate);
        return -EPERM;
    }

    for (alias_idx = 0; alias_idx < candidate->alias_count; alias_idx++) {
        struct cxl_alias *alias = &candidate->aliases[alias_idx];
        struct vmregion *vmr;
        int mid;

        read_lock(&alias->vmspace->vmspace_lock);
        alias->read_locked = true;
        lock(&alias->vmspace->pgtbl_lock);
        if (!cxl_policy_allows_mapping(candidate->pmo, alias->perm)) {
            unlock(&alias->vmspace->pgtbl_lock);
            restore_candidate_ptes(candidate);
            release_candidate_aliases(candidate);
            return -EPERM;
        }
        vmr = find_vmr_for_va(alias->vmspace, alias->va);
        if (!vmr || vmr->pmo != candidate->pmo
            || (alias->va - vmr->start) / PAGE_SIZE
                       != candidate->pmo_index) {
            unlock(&alias->vmspace->pgtbl_lock);
            continue;
        }

        if (get_page_from_pmo(candidate->pmo, candidate->pmo_index)
            != candidate->cxl_pa) {
            unlock(&alias->vmspace->pgtbl_lock);
            restore_candidate_ptes(candidate);
            release_candidate_aliases(candidate);
            return -EAGAIN;
        }

        for (mid = 0; mid < CLUSTER_MACHINE_NUM; mid++) {
            void *pgtbl = get_vmspace_pgtbl(alias->vmspace, mid);
            paddr_t pa = 0;
            pte_t *pte = NULL;
            struct cxl_saved_mapping *mapping;

            if (!pgtbl)
                continue;
            query_in_pgtbl(pgtbl, alias->va, &pa, &pte);
            if (!pte || !pte->pte_4K.present)
                continue;
            if (is_migration_entry(pte) || pa != candidate->cxl_pa
                || candidate->mapping_count
                           == CXL_DEMOTE_MAX_MAPPINGS_PER_PAGE) {
                unlock(&alias->vmspace->pgtbl_lock);
                restore_candidate_ptes(candidate);
                release_candidate_aliases(candidate);
                return -EAGAIN;
            }
            mapping = &candidate->mappings[candidate->mapping_count++];
            mapping->pte = pte;
            mapping->old_pteval = pte->pteval;
            mapping->machine_id = mid;
            mapping->vmspace = alias->vmspace;
            mapping->va = alias->va;
            /*
             * A naturally cold page may have been touched after its aging
             * observation.  Abort before disturbing more mappings.  The tiny
             * race window on mappings already claimed is restored locally
             * and never reaches the distributed flush phase.
             */
            if (mapping->old_pteval & PAGE_ACCESSED) {
                unlock(&alias->vmspace->pgtbl_lock);
                restore_candidate_ptes(candidate);
                release_candidate_aliases(candidate);
                return -EAGAIN;
            }
            set_migration_entry(pte);
        }
        unlock(&alias->vmspace->pgtbl_lock);
    }

    candidate->prepared = true;
    lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    if (candidate->page->cxl_reclaim_state == CXL_RECLAIM_DEMOTING)
        candidate->page->cxl_reclaim_phase = CXL_RECLAIM_PHASE_PREPARED;
    unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    return 0;
}

static bool operation_matches_page(struct cxl_demote_batch_op *op,
                                   struct page **page_out,
                                   u64 phase)
{
    struct page *page = validated_cxl_page(op->src_pa);
    bool valid = false;

    if (!page)
        return false;

    lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    if (page->cxl_sequence != op->txn_id
        || page->cxl_origin_pa != op->dst_pa
        || page->cxl_owner_mid < 0
        || page->cxl_owner_mid >= CLUSTER_MACHINE_NUM)
        goto out;

    if (phase == CXL_DEMOTE_PHASE_FLUSH) {
        valid = page->cxl_reclaim_state == CXL_RECLAIM_DEMOTING
                && page->cxl_reclaim_phase == CXL_RECLAIM_PHASE_PREPARED;
    } else if (phase == CXL_DEMOTE_PHASE_COPY) {
        valid = page->cxl_reclaim_state == CXL_RECLAIM_DEMOTING
                && page->cxl_reclaim_phase == CXL_RECLAIM_PHASE_FLUSHED
                && page->cxl_owner_mid == CUR_MACHINE_ID;
    } else if (phase == CXL_DEMOTE_PHASE_FREE_ORIGIN) {
        valid = (page->cxl_reclaim_state == CXL_RECLAIM_FREE_PENDING
                 || page->cxl_reclaim_state == CXL_RECLAIM_FREEING)
                && page->cxl_owner_mid == CUR_MACHINE_ID;
    } else if (phase == CXL_DEMOTE_PHASE_BITMAP_DRAM) {
        valid = page->cxl_reclaim_state == CXL_RECLAIM_DEMOTING
                && page->cxl_reclaim_phase == CXL_RECLAIM_PHASE_FLUSHED;
    }
out:
    unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    if (valid)
        *page_out = page;
    return valid;
}

static struct vmspace *validated_flush_vmspace(struct page *page,
                                               struct cxl_demote_batch_op *op)
{
    struct pmobject *pmo = (struct pmobject *)page->cxl_pmo;
    struct vmregion *vmr;
    struct vmspace *vmspace = NULL;

    if (!pmo)
        return NULL;
    cxl_pmo_mapping_init(pmo);
    lock(&pmo->cxl_mapping_lock);
    for_each_in_list (vmr,
                      struct vmregion,
                      cxl_pmo_node,
                      &pmo->cxl_mapping_list) {
        if (vmr->cxl_pmo_linked && vmr->pmo == pmo
            && (u64)vmr->vmspace == op->vmspace_ptr
            && page->cxl_pmo_index < DIV_ROUND_UP(vmr->size, PAGE_SIZE)
            && vmr->start + page->cxl_pmo_index * PAGE_SIZE
                       == op->fault_va) {
            vmspace = (struct vmspace *)vmr->vmspace;
            if (!cxl_object_try_get(vmspace))
                vmspace = NULL;
            break;
        }
    }
    unlock(&pmo->cxl_mapping_lock);
    return vmspace;
}

static bool operation_matches_aging(struct cxl_demote_batch_op *op,
                                    struct page **page_out)
{
    struct page *page = validated_cxl_page(op->src_pa);
    bool valid;

    if (!page)
        return false;
    lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    valid = page->cxl_sequence == op->txn_id
            && page->cxl_origin_pa == op->dst_pa
            && page->cxl_reclaim_state == CXL_RECLAIM_RESIDENT
            && page->cxl_scanning
            && cxl_policy_allows_pmo((struct pmobject *)page->cxl_pmo);
    unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    if (valid)
        *page_out = page;
    return valid;
}

int dsm_cxl_handle_aging_batch(struct cxl_demote_batch_op *ops,
                               u64 ops_count)
{
    struct tlb_flush_batch_op tlb_ops[CXL_DEMOTE_MAX_BATCH];
    struct vmspace *vmspaces[CXL_DEMOTE_MAX_BATCH] = { 0 };
    struct page *pages[CXL_DEMOTE_MAX_BATCH] = { 0 };
    u64 i;
    int ret = 0;

    if (!dsm_cxl_reclaim_enabled())
        return -EOPNOTSUPP;
    if (!ops || ops_count == 0 || ops_count > CXL_DEMOTE_MAX_BATCH)
        return -EINVAL;

    for (i = 0; i < ops_count; i++) {
        void *pgtbl;
        paddr_t pa = 0;
        pte_t *pte = NULL;

        if (!operation_matches_aging(&ops[i], &pages[i])) {
            ret = -EPERM;
            goto out;
        }
        vmspaces[i] = validated_flush_vmspace(pages[i], &ops[i]);
        if (!vmspaces[i]) {
            ret = -EAGAIN;
            goto out;
        }
        pgtbl = get_vmspace_pgtbl(vmspaces[i], CUR_MACHINE_ID);
        if (!pgtbl) {
            ret = -EAGAIN;
            goto out;
        }
        lock(&vmspaces[i]->pgtbl_lock);
        query_in_pgtbl(pgtbl, ops[i].fault_va, &pa, &pte);
        if (!pte || !pte->pte_4K.present || is_migration_entry(pte)
            || pa != ops[i].src_pa) {
            unlock(&vmspaces[i]->pgtbl_lock);
            ret = -EAGAIN;
            goto out;
        }
        tlb_ops[i].fault_va = ops[i].fault_va;
        tlb_ops[i].len = PAGE_SIZE;
        tlb_ops[i].pcid = get_pcid(pgtbl);
        tlb_ops[i].vmspace_ptr = ops[i].vmspace_ptr;
        unlock(&vmspaces[i]->pgtbl_lock);
    }

    flush_tlbs_batch_on_all_cpus(tlb_ops, ops_count);
out:
    for (i = 0; i < ops_count; i++) {
        if (vmspaces[i])
            obj_put(vmspaces[i]);
    }
    return ret;
}

int dsm_cxl_handle_batch(struct cxl_demote_batch_op *ops, u64 ops_count,
                         u64 phase)
{
    struct tlb_flush_batch_op tlb_ops[CXL_DEMOTE_MAX_BATCH];
    struct vmspace *vmspaces[CXL_DEMOTE_MAX_BATCH] = { 0 };
    struct page *pages[CXL_DEMOTE_MAX_BATCH] = { 0 };
    u64 i;
    int ret = 0;

    if (!dsm_cxl_reclaim_enabled())
        return -EOPNOTSUPP;
    if (!ops || ops_count == 0 || ops_count > CXL_DEMOTE_MAX_BATCH)
        return -EINVAL;
    if (phase > CXL_DEMOTE_PHASE_BITMAP_DRAM)
        return -EINVAL;

    for (i = 0; i < ops_count; i++) {
        if (!operation_matches_page(&ops[i], &pages[i], phase)) {
            ret = -EPERM;
            goto out;
        }
        if (!IS_LOCAL_PADDR(ops[i].dst_pa, pages[i]->cxl_owner_mid)) {
            ret = -EINVAL;
            goto out;
        }
        if (phase == CXL_DEMOTE_PHASE_COPY
            || (phase == CXL_DEMOTE_PHASE_FREE_ORIGIN
                && __atomic_load_n(&pages[i]->cxl_origin_release_state,
                                   __ATOMIC_ACQUIRE)
                           != CXL_ORIGIN_RELEASED)) {
            struct page *origin_page =
                    virt_to_page((void *)phys_to_virt(ops[i].dst_pa));

            if (!origin_page || origin_page->pool->type != DRAM_PAGE
                || !page_check_flag(origin_page, PG_allocated)) {
                ret = -EINVAL;
                goto out;
            }
            if (phase == CXL_DEMOTE_PHASE_COPY
                && __atomic_load_n(&origin_page->ref_cnt, __ATOMIC_ACQUIRE)
                           != 1) {
                ret = -EBUSY;
                goto out;
            }
        }

        if (phase == CXL_DEMOTE_PHASE_FLUSH
            || phase == CXL_DEMOTE_PHASE_BITMAP_DRAM) {
            void *pgtbl;
            paddr_t pa = 0;
            pte_t *pte = NULL;

            vmspaces[i] = validated_flush_vmspace(pages[i], &ops[i]);
            if (!vmspaces[i]) {
                ret = -EPERM;
                goto out;
            }
            if (phase == CXL_DEMOTE_PHASE_BITMAP_DRAM)
                continue;

            pgtbl = get_vmspace_pgtbl(vmspaces[i], CUR_MACHINE_ID);
            if (!pgtbl) {
                ret = -EINVAL;
                goto out;
            }
            lock(&vmspaces[i]->pgtbl_lock);
            query_in_pgtbl(pgtbl, ops[i].fault_va, &pa, &pte);
            if (!pte || !is_migration_entry(pte)) {
                unlock(&vmspaces[i]->pgtbl_lock);
                ret = -EAGAIN;
                goto out;
            }
            tlb_ops[i].fault_va = ops[i].fault_va;
            tlb_ops[i].len = PAGE_SIZE;
            tlb_ops[i].pcid = get_pcid(pgtbl);
            tlb_ops[i].vmspace_ptr = ops[i].vmspace_ptr;
            unlock(&vmspaces[i]->pgtbl_lock);
        }
    }

    if (phase == CXL_DEMOTE_PHASE_FLUSH) {
        flush_tlbs_batch_on_all_cpus(tlb_ops, ops_count);
    } else if (phase == CXL_DEMOTE_PHASE_COPY) {
        for (i = 0; i < ops_count; i++)
            memcpy((void *)phys_to_virt(ops[i].dst_pa),
                   (void *)phys_to_virt(ops[i].src_pa),
                   PAGE_SIZE);
    } else if (phase == CXL_DEMOTE_PHASE_BITMAP_DRAM) {
        for (i = 0; i < ops_count; i++)
            cxlprof_live_mark_dram(vmspaces[i], ops[i].fault_va);
    } else {
        for (i = 0; i < ops_count; i++) {
            u8 expected = CXL_ORIGIN_NOT_RELEASED;

            if (__atomic_compare_exchange_n(
                        &pages[i]->cxl_origin_release_state,
                        &expected,
                        CXL_ORIGIN_RELEASING,
                        false,
                        __ATOMIC_ACQ_REL,
                        __ATOMIC_ACQUIRE)) {
                kfree((void *)phys_to_virt(ops[i].dst_pa));
                __atomic_store_n(&pages[i]->cxl_origin_release_state,
                                 CXL_ORIGIN_RELEASED,
                                 __ATOMIC_RELEASE);
            } else if (expected == CXL_ORIGIN_RELEASING) {
                ret = -EAGAIN;
                goto out;
            }
        }
    }

out:
    for (i = 0; i < ops_count; i++) {
        if (vmspaces[i])
            obj_put(vmspaces[i]);
    }
    return ret;
}

static bool cancel_unclaimed_control_request(mid_t target_mid, u64 rpc_id)
{
    bool cancelled = false;

    lock(&dsm_meta->cxl_control[target_mid].lock);
    if (dsm_meta->cxl_control[target_mid].rpc_id == rpc_id
        && dsm_meta->cxl_control[target_mid].sender == CUR_MACHINE_ID
        && dsm_meta->cxl_control[target_mid].pending == 1) {
        __atomic_store_n(&dsm_meta->cxl_control[target_mid].pending,
                         0,
                         __ATOMIC_RELEASE);
        dsm_meta->cxl_control[target_mid].sender = 0xFFFFFFFF;
        cancelled = true;
    }
    unlock(&dsm_meta->cxl_control[target_mid].lock);
    return cancelled;
}

static int send_control_batch(mid_t target_mid,
                              struct cxl_demote_batch_op *ops,
                              u32 count, u32 kind, u32 phase)
{
    mid_t my_id = CUR_MACHINE_ID;
    u64 rpc_id = __atomic_add_fetch(&dsm_meta->cxl_reclaim.next_rpc_id,
                                    1,
                                    __ATOMIC_ACQ_REL);
    u64 deadline;
    u32 polls = 0;
    u32 i;
    int result;

    /*
     * The control mailbox is independent of both the foreground durable queue
     * and the legacy MSI/TLB slot.  Bound admission to the cluster-serialized
     * wire slot; a failed attempt simply defers asynchronous reclaim.
     */
    deadline = plat_get_mono_time() + DSM_CXL_RPC_TIMEOUT_NS;
    while (try_lock(&dsm_meta->cxl_control_rpc_lock) != 0) {
        if (plat_get_mono_time() >= deadline)
            return -EAGAIN;
        CPU_PAUSE();
    }

    __atomic_store_n(&dsm_meta->cxl_control[my_id].reply_rpc_id,
                     0,
                     __ATOMIC_RELEASE);

    lock(&dsm_meta->cxl_control[target_mid].lock);
    if (__atomic_load_n(&dsm_meta->cxl_control[target_mid].pending,
                        __ATOMIC_ACQUIRE)) {
        unlock(&dsm_meta->cxl_control[target_mid].lock);
        unlock(&dsm_meta->cxl_control_rpc_lock);
        return -EAGAIN;
    }
    dsm_meta->cxl_control[target_mid].sender = my_id;
    dsm_meta->cxl_control[target_mid].kind = kind;
    dsm_meta->cxl_control[target_mid].phase = phase;
    dsm_meta->cxl_control[target_mid].count = count;
    dsm_meta->cxl_control[target_mid].rpc_id = rpc_id;
    for (i = 0; i < count; i++) {
        dsm_meta->cxl_control[target_mid].ops[i].src_pa =
                ops[i].src_pa;
        dsm_meta->cxl_control[target_mid].ops[i].dst_pa =
                ops[i].dst_pa;
        dsm_meta->cxl_control[target_mid].ops[i].fault_va =
                ops[i].fault_va;
        dsm_meta->cxl_control[target_mid].ops[i].vmspace_ptr =
                ops[i].vmspace_ptr;
        dsm_meta->cxl_control[target_mid].ops[i].txn_id =
                ops[i].txn_id;
    }
    __atomic_store_n(&dsm_meta->cxl_control[target_mid].pending,
                     1,
                     __ATOMIC_RELEASE);
    unlock(&dsm_meta->cxl_control[target_mid].lock);

    deadline = plat_get_mono_time() + DSM_CXL_RPC_TIMEOUT_NS;
    while (1) {
        extern void handle_ipi(void);

        if (__atomic_load_n(&dsm_meta->cxl_control[my_id].reply_rpc_id,
                            __ATOMIC_ACQUIRE)
            == rpc_id) {
            result = __atomic_load_n(&dsm_meta->cxl_control[my_id].result,
                                     __ATOMIC_RELAXED);
            unlock(&dsm_meta->cxl_control_rpc_lock);
            return result;
        }
        if ((++polls & 1023U) == 0 && plat_get_mono_time() >= deadline)
            break;
        handle_ipi();
        CPU_PAUSE();
    }

    if (cancel_unclaimed_control_request(target_mid, rpc_id)) {
        unlock(&dsm_meta->cxl_control_rpc_lock);
        return -ETIMEDOUT;
    }

    /*
     * pending == 2 transfers ownership to the target worker.  Cancelling at
     * that point would let a late copy/bitmap phase run after the caller has
     * restored PTEs and transaction state.  This is an asynchronous demoter
     * thread, not a faulting thread; drain the already accepted, finite local
     * operation so the caller can make the all-or-nothing decision safely.
     */
    while (__atomic_load_n(&dsm_meta->cxl_control[my_id].reply_rpc_id,
                           __ATOMIC_ACQUIRE)
           != rpc_id) {
        handle_ipi();
        CPU_PAUSE();
    }
    result = __atomic_load_n(&dsm_meta->cxl_control[my_id].result,
                             __ATOMIC_RELAXED);
    unlock(&dsm_meta->cxl_control_rpc_lock);
    return result;
}

static int send_remote_batch(mid_t target_mid,
                             struct cxl_demote_batch_op *ops,
                             u32 count, u32 phase)
{
    if (target_mid == CUR_MACHINE_ID)
        return dsm_cxl_handle_batch(ops, count, phase);
    /*
     * Foreground DRAM-to-CXL promotion uses the target machine's polling
     * queue.  Demotion control traffic must not occupy that single reader or
     * sit ahead of a page fault, so use the independent reclaim-worker
     * mailbox.  It also cannot be overwritten by legacy MSI/TLB traffic.
     */
    return send_control_batch(target_mid,
                              ops,
                              count,
                              CXL_CONTROL_TXN_DEMOTE,
                              phase);
}

#ifdef DSM_CXL_DEMOTE_CLOCK
static int send_aging_batch(mid_t target_mid,
                            struct cxl_demote_batch_op *ops, u32 count)
{
    if (target_mid == CUR_MACHINE_ID)
        return dsm_cxl_handle_aging_batch(ops, count);
    return send_control_batch(target_mid,
                              ops,
                              count,
                              CXL_CONTROL_TXN_AGING,
                              0);
}

static int flush_aging_candidates(struct cxl_demote_candidate *batch,
                                  u32 count, u32 *batch_count_out,
                                  u32 *machine_count_out)
{
    struct cxl_demote_batch_op ops[CXL_DEMOTE_MAX_BATCH];
    u32 batches = 0;
    u32 machines = 0;
    mid_t mid;

    for (mid = 0; mid < CLUSTER_MACHINE_NUM; mid++) {
        u32 i, mapping_idx, op_count = 0;
        bool machine_touched = false;

        for (i = 0; i < count; i++) {
            for (mapping_idx = 0;
                 mapping_idx < batch[i].aged_mapping_count;
                 mapping_idx++) {
                struct cxl_aged_mapping *mapping =
                        &batch[i].aged_mappings[mapping_idx];

                if (mapping->machine_id != mid)
                    continue;
                ops[op_count].src_pa = batch[i].cxl_pa;
                ops[op_count].dst_pa = batch[i].origin_pa;
                ops[op_count].fault_va = mapping->va;
                ops[op_count].vmspace_ptr = (u64)mapping->vmspace;
                ops[op_count].txn_id = batch[i].page->cxl_sequence;
                op_count++;
                if (op_count == CXL_DEMOTE_MAX_BATCH) {
                    if (send_aging_batch(mid, ops, op_count))
                        return -EIO;
                    batches++;
                    machine_touched = true;
                    op_count = 0;
                }
            }
        }
        if (op_count) {
            if (send_aging_batch(mid, ops, op_count))
                return -EIO;
            batches++;
            machine_touched = true;
        }
        if (machine_touched)
            machines++;
    }
    *batch_count_out = batches;
    *machine_count_out = machines;
    return 0;
}

static void record_aging_stats(u64 latency_ns, u32 pages, u32 batches,
                               u32 machines)
{
    u64 total_batches;
    u64 old_max;

    total_batches = __atomic_add_fetch(
            &dsm_meta->cxl_reclaim.aging_flush_batches,
            batches,
            __ATOMIC_RELAXED);
    __atomic_add_fetch(&dsm_meta->cxl_reclaim.aging_flush_pages,
                       pages,
                       __ATOMIC_RELAXED);
    __atomic_add_fetch(&dsm_meta->cxl_reclaim.aging_flush_machines,
                       machines,
                       __ATOMIC_RELAXED);
    __atomic_add_fetch(&dsm_meta->cxl_reclaim.aging_flush_latency_ns,
                       latency_ns,
                       __ATOMIC_RELAXED);
    old_max = __atomic_load_n(&dsm_meta->cxl_reclaim.aging_flush_max_ns,
                              __ATOMIC_RELAXED);
    while (latency_ns > old_max
           && !__atomic_compare_exchange_n(
                   &dsm_meta->cxl_reclaim.aging_flush_max_ns,
                   &old_max,
                   latency_ns,
                   false,
                   __ATOMIC_RELAXED,
                   __ATOMIC_RELAXED))
        ;

    if (total_batches == batches
        || total_batches / CXL_AGING_REPORT_INTERVAL
                   != (total_batches - batches)
                              / CXL_AGING_REPORT_INTERVAL) {
        kinfo("[CXL_AGING] batches=%lu pages=%lu machines=%lu "
              "latency_ns=%lu total_latency_ns=%lu max_latency_ns=%lu\n",
              total_batches,
              __atomic_load_n(&dsm_meta->cxl_reclaim.aging_flush_pages,
                              __ATOMIC_RELAXED),
              __atomic_load_n(&dsm_meta->cxl_reclaim.aging_flush_machines,
                              __ATOMIC_RELAXED),
              latency_ns,
              __atomic_load_n(&dsm_meta->cxl_reclaim.aging_flush_latency_ns,
                              __ATOMIC_RELAXED),
              __atomic_load_n(&dsm_meta->cxl_reclaim.aging_flush_max_ns,
                              __ATOMIC_RELAXED));
        kinfo("[CXL_ELIGIBLE] validated_pmo_anonym=%lu "
              "validated_pmo_data=%lu validated_pmo_shm=%lu "
              "pmo_stack=%lu pmo_heap=%lu perm_none=%lu perm_r=%lu "
              "perm_w=%lu perm_rw=%lu\n",
              __atomic_load_n(
                      &dsm_meta->cxl_reclaim
                               .eligible_by_pmo_type[PMO_ANONYM],
                      __ATOMIC_RELAXED),
              __atomic_load_n(
                      &dsm_meta->cxl_reclaim
                               .eligible_by_pmo_type[PMO_DATA],
                      __ATOMIC_RELAXED),
              __atomic_load_n(
                      &dsm_meta->cxl_reclaim
                               .eligible_by_pmo_type[PMO_SHM],
                      __ATOMIC_RELAXED),
              __atomic_load_n(
                      &dsm_meta->cxl_reclaim
                               .eligible_by_pmo_type[PMO_STACK],
                      __ATOMIC_RELAXED),
              __atomic_load_n(
                      &dsm_meta->cxl_reclaim
                               .eligible_by_pmo_type[PMO_HEAP],
                      __ATOMIC_RELAXED),
              __atomic_load_n(&dsm_meta->cxl_reclaim.eligible_by_perm[0],
                              __ATOMIC_RELAXED),
              __atomic_load_n(
                      &dsm_meta->cxl_reclaim.eligible_by_perm[VMR_READ],
                      __ATOMIC_RELAXED),
              __atomic_load_n(
                      &dsm_meta->cxl_reclaim.eligible_by_perm[VMR_WRITE],
                      __ATOMIC_RELAXED),
              __atomic_load_n(
                      &dsm_meta->cxl_reclaim.eligible_by_perm[
                              VMR_READ | VMR_WRITE],
                      __ATOMIC_RELAXED));
    }
}
#endif

static void fill_candidate_op(struct cxl_demote_batch_op *op,
                              struct cxl_demote_candidate *candidate,
                              struct cxl_alias *alias)
{
    op->src_pa = candidate->cxl_pa;
    op->dst_pa = candidate->origin_pa;
    op->fault_va = alias ? alias->va : 0;
    op->vmspace_ptr = alias ? (u64)alias->vmspace : 0;
    op->txn_id = candidate->page->cxl_sequence;
}

static int flush_candidates(struct cxl_demote_candidate *batch, u32 count)
{
    struct cxl_demote_batch_op ops[CXL_DEMOTE_MAX_BATCH];
    mid_t mid;

    for (mid = 0; mid < CLUSTER_MACHINE_NUM; mid++) {
        u32 i, alias_idx, op_count = 0;

        for (i = 0; i < count; i++) {
            for (alias_idx = 0; alias_idx < batch[i].alias_count;
                 alias_idx++) {
                struct cxl_alias *alias = &batch[i].aliases[alias_idx];
                u32 mapping_idx;
                bool mapped = false;

                if (!alias->vmspace)
                    continue;
                for (mapping_idx = 0;
                     mapping_idx < batch[i].mapping_count;
                     mapping_idx++) {
                    struct cxl_saved_mapping *mapping =
                            &batch[i].mappings[mapping_idx];

                    if (mapping->machine_id == mid
                        && mapping->vmspace == alias->vmspace
                        && mapping->va == alias->va) {
                        mapped = true;
                        break;
                    }
                }
                if (!mapped)
                    continue;

                fill_candidate_op(&ops[op_count++], &batch[i], alias);
                if (op_count == CXL_DEMOTE_MAX_BATCH) {
                    if (send_remote_batch(mid,
                                          ops,
                                          op_count,
                                          CXL_DEMOTE_PHASE_FLUSH))
                        return -EIO;
                    op_count = 0;
                }
            }
        }
        if (op_count
            && send_remote_batch(mid,
                                 ops,
                                 op_count,
                                 CXL_DEMOTE_PHASE_FLUSH))
            return -EIO;
    }
    return 0;
}

static void mark_candidates_flushed(struct cxl_demote_candidate *batch,
                                    u32 count)
{
    u32 i;

    lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    for (i = 0; i < count; i++) {
        if (batch[i].page->cxl_reclaim_state == CXL_RECLAIM_DEMOTING
            && batch[i].page->cxl_reclaim_phase
                       == CXL_RECLAIM_PHASE_PREPARED)
            batch[i].page->cxl_reclaim_phase = CXL_RECLAIM_PHASE_FLUSHED;
    }
    unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
}

static int copy_candidates(struct cxl_demote_candidate *batch, u32 count)
{
    struct cxl_demote_batch_op ops[CXL_DEMOTE_MAX_BATCH];
    mid_t mid;

    for (mid = 0; mid < CLUSTER_MACHINE_NUM; mid++) {
        u32 i, op_count = 0;

        for (i = 0; i < count; i++) {
            if (batch[i].owner_mid != mid)
                continue;
            fill_candidate_op(&ops[op_count++], &batch[i], NULL);
        }
        if (op_count
            && send_remote_batch(mid,
                                 ops,
                                 op_count,
                                 CXL_DEMOTE_PHASE_COPY))
            return -EIO;
    }
    return 0;
}

static int clear_candidate_bitmaps(struct cxl_demote_candidate *batch,
                                   u32 count)
{
    struct cxl_demote_batch_op ops[CXL_DEMOTE_MAX_BATCH];
    mid_t mid;

    for (mid = 0; mid < CLUSTER_MACHINE_NUM; mid++) {
        u32 i, mapping_idx, op_count = 0;

        for (i = 0; i < count; i++) {
            for (mapping_idx = 0;
                 mapping_idx < batch[i].mapping_count;
                 mapping_idx++) {
                struct cxl_saved_mapping *mapping =
                        &batch[i].mappings[mapping_idx];

                if (mapping->machine_id != mid)
                    continue;
                ops[op_count].src_pa = batch[i].cxl_pa;
                ops[op_count].dst_pa = batch[i].origin_pa;
                ops[op_count].fault_va = mapping->va;
                ops[op_count].vmspace_ptr = (u64)mapping->vmspace;
                ops[op_count].txn_id = batch[i].page->cxl_sequence;
                op_count++;
                if (op_count == CXL_DEMOTE_MAX_BATCH) {
                    if (send_remote_batch(mid,
                                          ops,
                                          op_count,
                                          CXL_DEMOTE_PHASE_BITMAP_DRAM))
                        return -EIO;
                    op_count = 0;
                }
            }
        }
        if (op_count
            && send_remote_batch(mid,
                                 ops,
                                 op_count,
                                 CXL_DEMOTE_PHASE_BITMAP_DRAM))
            return -EIO;
    }
    return 0;
}

static void finalize_cxl_page_free(struct page *page)
{
    lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    if ((page->cxl_reclaim_state != CXL_RECLAIM_FREE_PENDING
         && page->cxl_reclaim_state != CXL_RECLAIM_FREEING)
        || __atomic_load_n(&page->cxl_origin_release_state, __ATOMIC_ACQUIRE)
                   != CXL_ORIGIN_RELEASED) {
        unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
        return;
    }

    if (page->cxl_reclaim_state == CXL_RECLAIM_FREE_PENDING) {
        BUG_ON(dsm_meta->cxl_reclaim.free_pending_pages == 0);
        dsm_meta->cxl_reclaim.free_pending_pages--;
    }
    cxl_sample_forget_node(&page->cxl_reclaim_node);
    list_del(&page->cxl_reclaim_node);
    set_cxl_reclaim_state(page, CXL_RECLAIM_NONE);
    page->cxl_reclaim_phase = CXL_RECLAIM_PHASE_IDLE;
    page->cxl_origin_pa = 0;
    page->cxl_pmo = 0;
    page->cxl_pmo_index = 0;
    page->cxl_free_requested = 0;
    page->cxl_scanning = 0;
    page->cxl_cold_epochs = 0;
    page->cxl_speculative = 0;
    page->cxl_age_armed = 0;
    page->cxl_policy_eligible = 0;
    page->cxl_policy_generation = 0;
    page->cxl_policy_perm = 0;
    page->cxl_age_epoch = 0;
    page->cxl_age_started_ns = 0;
    page->cxl_promoted_ns = 0;
    unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);

    /* Make the physical page available before advertising new headroom. */
    buddy_free_pages(page->pool, page);

    lock(&dsm_meta->cxl_reclaim.account_guard.lock);
    BUG_ON(dsm_meta->cxl_reclaim.allocated_pages == 0);
    BUG_ON(dsm_meta->cxl_reclaim.resident_pages == 0);
    dsm_meta->cxl_reclaim.allocated_pages--;
    dsm_meta->cxl_reclaim.resident_pages--;
    unlock(&dsm_meta->cxl_reclaim.account_guard.lock);
}

static void defer_origin_release(struct page *page)
{
    lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    if (page->cxl_reclaim_state == CXL_RECLAIM_FREEING) {
        set_cxl_reclaim_state(page, CXL_RECLAIM_FREE_PENDING);
        dsm_meta->cxl_reclaim.free_pending_pages++;
        cxl_sample_forget_node(&page->cxl_reclaim_node);
        list_del(&page->cxl_reclaim_node);
        /*
         * Park pending frees at the head, not the tail: retry_pending_frees()
         * scans from the head and would otherwise have to walk the entire
         * resident set to reach them.  Eviction order is unaffected because
         * select_candidates() only picks CXL_RECLAIM_RESIDENT entries.
         */
        list_add(&page->cxl_reclaim_node, &dsm_meta->cxl_reclaim.fifo);
    }
    unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
}

static int release_origin_page(struct page *page)
{
    struct cxl_demote_batch_op op;
    mid_t owner_mid;
    int ret;

    lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    if (page->cxl_reclaim_state != CXL_RECLAIM_FREE_PENDING
        && page->cxl_reclaim_state != CXL_RECLAIM_FREEING) {
        unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
        return -EINVAL;
    }
    if (page->cxl_reclaim_state == CXL_RECLAIM_FREE_PENDING) {
        BUG_ON(dsm_meta->cxl_reclaim.free_pending_pages == 0);
        dsm_meta->cxl_reclaim.free_pending_pages--;
    }
    set_cxl_reclaim_state(page, CXL_RECLAIM_FREEING);
    op.src_pa = cxl_page_pa(page);
    op.dst_pa = page->cxl_origin_pa;
    op.fault_va = 0;
    op.vmspace_ptr = 0;
    op.txn_id = page->cxl_sequence;
    owner_mid = page->cxl_owner_mid;
    unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);

    ret = send_remote_batch(owner_mid,
                            &op,
                            1,
                            CXL_DEMOTE_PHASE_FREE_ORIGIN);
    if (__atomic_load_n(&page->cxl_origin_release_state, __ATOMIC_ACQUIRE)
        == CXL_ORIGIN_RELEASED)
        ret = 0;

    if (ret == 0) {
        finalize_cxl_page_free(page);
        return 1;
    }

    defer_origin_release(page);
    return ret;
}

void dsm_cxl_free_page(struct page *page)
{
    u64 pages;

    BUG_ON(!page || page->pool->type != CXL_MEM_PAGE);
    if (!dsm_meta
        || !__atomic_load_n(&dsm_meta->cxl_reclaim.initialized,
                            __ATOMIC_ACQUIRE)) {
        buddy_free_pages(page->pool, page);
        return;
    }
    pages = 1UL << page->order;

    lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    if (!page_check_flag(page, PG_allocated)) {
        unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
        return;
    }
    if (page->cxl_scanning
        || page->cxl_reclaim_state == CXL_RECLAIM_DEMOTING) {
        page->cxl_free_requested = 1;
        unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
        return;
    }
    if (page->cxl_reclaim_state == CXL_RECLAIM_FREE_PENDING
        || page->cxl_reclaim_state == CXL_RECLAIM_FREEING) {
        unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
        return;
    }
    if (page->cxl_reclaim_state == CXL_RECLAIM_RESIDENT) {
        /*
         * Object teardown is a foreground path and may release millions of
         * tracked pages.  Never perform an owner RPC here.  Park the page at
         * the queue head and let the reclaim pthread coalesce origin releases
         * by owner in retry_pending_frees().
         */
        set_cxl_reclaim_state(page, CXL_RECLAIM_FREE_PENDING);
        page->cxl_reclaim_phase = CXL_RECLAIM_PHASE_IDLE;
        unaccount_page_policy_locked(page,
                                     (struct pmobject *)page->cxl_pmo);
        page->cxl_pmo = 0;
        dsm_meta->cxl_reclaim.free_pending_pages++;
        cxl_sample_forget_node(&page->cxl_reclaim_node);
        list_del(&page->cxl_reclaim_node);
        list_add(&page->cxl_reclaim_node, &dsm_meta->cxl_reclaim.fifo);
        unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
        return;
    }

    buddy_free_pages(page->pool, page);
    lock(&dsm_meta->cxl_reclaim.account_guard.lock);
    BUG_ON(dsm_meta->cxl_reclaim.allocated_pages < pages);
    dsm_meta->cxl_reclaim.allocated_pages -= pages;
    unlock(&dsm_meta->cxl_reclaim.account_guard.lock);
    unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
}

static int retry_pending_frees(u32 limit)
{
    struct page *pages[CXL_DEMOTE_MAX_BATCH];
    struct cxl_demote_batch_op ops[CXL_DEMOTE_MAX_BATCH];
    u32 page_count = 0;
    int freed = 0;
    mid_t mid;

    limit = MIN(limit, CXL_DEMOTE_MAX_BATCH);

    /*
     * The FIFO holds every page resident in CXL, including temporary soft-limit
     * overcommit.  Walking it costs one remote cache miss per entry
     * and is done under the cluster-wide reclaim lock, so a walk that finds
     * nothing is not merely wasted work: it blocks every other machine's
     * allocation and fault accounting for the whole scan.  Pending frees are
     * rare, so check the counter before touching the list at all.
     */
    if (__atomic_load_n(&dsm_meta->cxl_reclaim.free_pending_pages,
                        __ATOMIC_RELAXED)
        == 0)
        return 0;

    lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    if (dsm_meta->cxl_reclaim.free_pending_pages != 0) {
        struct list_head *head = &dsm_meta->cxl_reclaim.fifo;
        struct list_head *node;
        u64 remaining = dsm_meta->cxl_reclaim.free_pending_pages;

        for (node = head->next; node != head && remaining;
             node = node->next) {
            struct page *candidate =
                    list_entry(node, struct page, cxl_reclaim_node);

            if (candidate->cxl_reclaim_state == CXL_RECLAIM_FREE_PENDING) {
                set_cxl_reclaim_state(candidate, CXL_RECLAIM_FREEING);
                BUG_ON(dsm_meta->cxl_reclaim.free_pending_pages == 0);
                dsm_meta->cxl_reclaim.free_pending_pages--;
                remaining--;
                pages[page_count++] = candidate;
                if (page_count == limit)
                    break;
            }
        }
    }
    unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);

    for (mid = 0; mid < CLUSTER_MACHINE_NUM; mid++) {
        struct page *op_pages[CXL_DEMOTE_MAX_BATCH];
        u32 i, op_count = 0;

        for (i = 0; i < page_count; i++) {
            if (pages[i]->cxl_owner_mid != mid)
                continue;
            ops[op_count].src_pa = cxl_page_pa(pages[i]);
            ops[op_count].dst_pa = pages[i]->cxl_origin_pa;
            ops[op_count].fault_va = 0;
            ops[op_count].vmspace_ptr = 0;
            ops[op_count].txn_id = pages[i]->cxl_sequence;
            op_pages[op_count++] = pages[i];
        }
        if (!op_count)
            continue;

        send_remote_batch(mid,
                          ops,
                          op_count,
                          CXL_DEMOTE_PHASE_FREE_ORIGIN);
        for (i = 0; i < op_count; i++) {
            if (__atomic_load_n(&op_pages[i]->cxl_origin_release_state,
                                __ATOMIC_ACQUIRE)
                == CXL_ORIGIN_RELEASED) {
                finalize_cxl_page_free(op_pages[i]);
                freed++;
            } else {
                defer_origin_release(op_pages[i]);
            }
        }
    }
    return freed;
}

static void release_candidate_pmo(struct cxl_demote_candidate *candidate)
{
    if (candidate->pmo_pinned) {
        obj_put(candidate->pmo);
        candidate->pmo_pinned = false;
    }
}

static void restore_candidates(struct cxl_demote_candidate *batch, u32 count)
{
    u32 i;

    for (i = 0; i < count; i++) {
        bool free_requested;

        if (batch[i].prepared)
            restore_candidate_ptes(&batch[i]);
        release_candidate_aliases(&batch[i]);
        lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
        if (batch[i].page->cxl_reclaim_state == CXL_RECLAIM_DEMOTING) {
            set_cxl_reclaim_state(batch[i].page, CXL_RECLAIM_RESIDENT);
            batch[i].page->cxl_reclaim_phase = CXL_RECLAIM_PHASE_IDLE;
        }
        free_requested = batch[i].page->cxl_free_requested != 0;
        unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
        if (free_requested)
            dsm_cxl_free_page(batch[i].page);
        release_candidate_pmo(&batch[i]);
    }
}

static void release_scanning_candidate(
        struct cxl_demote_candidate *candidate)
{
    bool free_requested = false;

    release_candidate_aliases(candidate);
    lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    if (candidate->page->cxl_scanning) {
        candidate->page->cxl_scanning = 0;
        free_requested = candidate->page->cxl_free_requested != 0;
    }
    unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    if (free_requested)
        dsm_cxl_free_page(candidate->page);
    release_candidate_pmo(candidate);
}

static bool claim_scanning_candidate(
        struct cxl_demote_candidate *candidate)
{
    bool claimed = false;

    lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    if (candidate->page->cxl_scanning
        && candidate->page->cxl_reclaim_state == CXL_RECLAIM_RESIDENT
        && get_page_from_pmo(candidate->pmo, candidate->pmo_index)
                   == candidate->cxl_pa) {
        candidate->page->cxl_scanning = 0;
        set_cxl_reclaim_state(candidate->page, CXL_RECLAIM_DEMOTING);
        candidate->page->cxl_reclaim_phase = CXL_RECLAIM_PHASE_SELECTED;
        claimed = true;
    }
    unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    return claimed;
}

static void record_protocol_stats(u64 prepare_ns, u64 flush_ns, u64 copy_ns,
                                  u64 bitmap_ns, u64 finish_ns, u64 total_ns,
                                  u32 pages)
{
    u64 transactions;
    u64 old_max;

    __atomic_add_fetch(&dsm_meta->cxl_reclaim.demote_phase_prepare_ns,
                       prepare_ns,
                       __ATOMIC_RELAXED);
    __atomic_add_fetch(&dsm_meta->cxl_reclaim.demote_phase_flush_ns,
                       flush_ns,
                       __ATOMIC_RELAXED);
    __atomic_add_fetch(&dsm_meta->cxl_reclaim.demote_phase_copy_ns,
                       copy_ns,
                       __ATOMIC_RELAXED);
    __atomic_add_fetch(&dsm_meta->cxl_reclaim.demote_phase_bitmap_ns,
                       bitmap_ns,
                       __ATOMIC_RELAXED);
    __atomic_add_fetch(&dsm_meta->cxl_reclaim.demote_phase_finish_ns,
                       finish_ns,
                       __ATOMIC_RELAXED);
    __atomic_add_fetch(&dsm_meta->cxl_reclaim.demote_phase_total_ns,
                       total_ns,
                       __ATOMIC_RELAXED);
    old_max = __atomic_load_n(&dsm_meta->cxl_reclaim.demote_phase_max_ns,
                              __ATOMIC_RELAXED);
    while (total_ns > old_max
           && !__atomic_compare_exchange_n(
                   &dsm_meta->cxl_reclaim.demote_phase_max_ns,
                   &old_max,
                   total_ns,
                   false,
                   __ATOMIC_RELAXED,
                   __ATOMIC_RELAXED))
        ;
    transactions = __atomic_add_fetch(
            &dsm_meta->cxl_reclaim.demote_transactions,
            1,
            __ATOMIC_RELAXED);
    if (transactions == 1
        || transactions % CXL_PROTOCOL_REPORT_INTERVAL == 0) {
        kinfo("[CXL_PROTOCOL] transactions=%lu pages=%u reclaimed=%lu "
              "promotions=%lu refaults=%lu conflicts=%lu "
              "last_ns prepare=%lu flush=%lu copy=%lu bitmap=%lu "
              "finish=%lu total=%lu max=%lu\n",
              transactions,
              pages,
              __atomic_load_n(&dsm_meta->cxl_reclaim.reclaimed_pages,
                              __ATOMIC_RELAXED),
              __atomic_load_n(&dsm_meta->cxl_reclaim.promotion_pages,
                              __ATOMIC_RELAXED),
              __atomic_load_n(&dsm_meta->cxl_reclaim.refault_pages,
                              __ATOMIC_RELAXED),
              __atomic_load_n(&dsm_meta->cxl_reclaim.demote_conflicts,
                              __ATOMIC_RELAXED),
              prepare_ns,
              flush_ns,
              copy_ns,
              bitmap_ns,
              finish_ns,
              total_ns,
              __atomic_load_n(&dsm_meta->cxl_reclaim.demote_phase_max_ns,
                              __ATOMIC_RELAXED));
    }
}

static void finish_candidate(struct cxl_demote_candidate *candidate)
{
    bool free_requested;
    bool free_cxl_page = false;
    u32 i;

    commit_page_to_pmo(candidate->pmo,
                       candidate->pmo_index,
                       candidate->origin_pa);
    free_requested = __atomic_load_n(&candidate->page->cxl_free_requested,
                                     __ATOMIC_ACQUIRE);
    for (i = 0; i < candidate->mapping_count; i++) {
        struct cxl_saved_mapping *mapping = &candidate->mappings[i];

        lock(&mapping->vmspace->pgtbl_lock);
        if (!free_requested && mapping->machine_id == candidate->owner_mid) {
            mapping->pte->pteval = mapping->old_pteval;
            remap_page_in_pgtbl(mapping->pte, candidate->origin_pa);
            mapping->pte->pte_4K.present = 1;
        } else {
            mapping->pte->pteval = 0;
        }
        unlock(&mapping->vmspace->pgtbl_lock);
    }
    if (free_requested)
        remove_page_from_pmo(candidate->pmo, candidate->pmo_index);

    release_candidate_aliases(candidate);

    lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    BUG_ON(candidate->page->cxl_reclaim_state != CXL_RECLAIM_DEMOTING);
    unaccount_page_policy_locked(candidate->page, candidate->pmo);
    if (free_requested) {
        set_cxl_reclaim_state(candidate->page, CXL_RECLAIM_FREEING);
        candidate->page->cxl_reclaim_phase = CXL_RECLAIM_PHASE_IDLE;
        candidate->page->cxl_pmo = 0;
    } else {
        cxl_sample_forget_node(&candidate->page->cxl_reclaim_node);
        list_del(&candidate->page->cxl_reclaim_node);
        set_cxl_reclaim_state(candidate->page, CXL_RECLAIM_NONE);
        candidate->page->cxl_reclaim_phase = CXL_RECLAIM_PHASE_IDLE;
        candidate->page->cxl_origin_pa = 0;
        candidate->page->cxl_pmo = 0;
        candidate->page->cxl_pmo_index = 0;
        candidate->page->cxl_free_requested = 0;
        candidate->page->cxl_scanning = 0;
        candidate->page->cxl_cold_epochs = 0;
        candidate->page->cxl_speculative = 0;
        candidate->page->cxl_age_armed = 0;
        candidate->page->cxl_policy_generation = 0;
        candidate->page->cxl_policy_perm = 0;
        candidate->page->cxl_age_epoch = 0;
        candidate->page->cxl_age_started_ns = 0;
        candidate->page->cxl_promoted_ns = 0;
        free_cxl_page = true;
    }
    unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);

    if (free_cxl_page) {
#ifdef DSM_CXL_DEMOTE_CLOCK
        u64 evictions;
#endif

        buddy_free_pages(candidate->page->pool, candidate->page);
        lock(&dsm_meta->cxl_reclaim.account_guard.lock);
        BUG_ON(dsm_meta->cxl_reclaim.allocated_pages == 0);
        BUG_ON(dsm_meta->cxl_reclaim.resident_pages == 0);
        dsm_meta->cxl_reclaim.allocated_pages--;
        dsm_meta->cxl_reclaim.resident_pages--;
        dsm_meta->cxl_reclaim.reclaimed_pages++;
        dsm_meta->cxl_reclaim.recent_demote_pa[
                (candidate->origin_pa >> PAGE_SHIFT)
                & (CXL_RECENT_DEMOTE_SLOTS - 1)] = candidate->origin_pa;
        unlock(&dsm_meta->cxl_reclaim.account_guard.lock);

#ifdef DSM_CXL_DEMOTE_CLOCK
        evictions = __atomic_add_fetch(
                &dsm_meta->cxl_reclaim.clock_cold_evictions,
                1,
                __ATOMIC_RELAXED);
        report_clock_stats(evictions, 1);
#endif
    }

    release_candidate_pmo(candidate);
    if (free_requested && release_origin_page(candidate->page) < 0)
        kwarn("[CXL_RECLAIM] deferred raced free pa=0x%lx\n",
              candidate->origin_pa);
}

static void defer_candidate(struct cxl_demote_candidate *candidate)
{
    bool free_requested;

    release_candidate_aliases(candidate);
    lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    if (candidate->page->cxl_reclaim_state == CXL_RECLAIM_DEMOTING) {
        set_cxl_reclaim_state(candidate->page, CXL_RECLAIM_RESIDENT);
        candidate->page->cxl_reclaim_phase = CXL_RECLAIM_PHASE_IDLE;
        cxl_sample_forget_node(&candidate->page->cxl_reclaim_node);
        list_del(&candidate->page->cxl_reclaim_node);
        list_append(&candidate->page->cxl_reclaim_node,
                    &dsm_meta->cxl_reclaim.fifo);
    }
    free_requested = candidate->page->cxl_free_requested != 0;
    unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    if (free_requested)
        dsm_cxl_free_page(candidate->page);
    release_candidate_pmo(candidate);
}

static int demote_one_batch(u32 limit, u64 now, bool allow_demote)
{
    u32 count, i, prepared_count = 0;
#ifdef DSM_CXL_DEMOTE_CLOCK
    u32 cold_count = 0;
    u32 second_chances = 0;
    u32 cooldown_skips = 0;
    u32 stable_cold = 0;
#endif
    u64 total_start = 0;
    u64 phase_start;
    u64 prepare_ns;
    u64 flush_ns;
    u64 copy_ns;
    u64 bitmap_ns;
    u64 finish_ns;
    int ret;

    count = select_candidates(limit);
    if (count == 0)
        return 0;

#ifdef DSM_CXL_DEMOTE_CLOCK
    {
    u32 age_count = 0;
    u32 aging_batches = 0;
    u32 aging_machines = 0;
    u32 armed = 0;
    u32 one_epoch_cold = 0;
    u64 aging_start;
    u64 completed_ns;
    u64 completed_cluster_ns;
    u64 epoch;

    /* Select only pages whose previous per-page epoch has fully elapsed. */
    for (i = 0; i < count; i++) {
        bool mature;

        lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
        mature = !candidates[i].page->cxl_age_armed
                 || (now >= candidates[i].page->cxl_age_started_ns
                     && now - candidates[i].page->cxl_age_started_ns
                                >= CXL_CLOCK_EPOCH_NS);
        unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
        if (!mature) {
            release_scanning_candidate(&candidates[i]);
            continue;
        }

        ret = prepare_candidate_aging(&candidates[i]);
        if (ret) {
            __atomic_add_fetch(&dsm_meta->cxl_reclaim.demote_conflicts,
                               1,
                               __ATOMIC_RELAXED);
            __atomic_add_fetch(&dsm_meta->cxl_reclaim.clock_scan_skips,
                               1,
                               __ATOMIC_RELAXED);
            if (ret == -EPERM || ret == -E2BIG || ret == -ENOENT) {
                lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
                account_page_policy_locked(candidates[i].page,
                                           candidates[i].pmo,
                                           false,
                                           0);
                reset_page_aging_locked(candidates[i].page,
                                         candidates[i].mapping_generation);
                unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
            }
            release_scanning_candidate(&candidates[i]);
            continue;
        }
        if (age_count != i)
            candidates[age_count] = candidates[i];
        age_count++;
    }

    if (age_count == 0)
        return 0;

    aging_start = plat_get_mono_time();
    ret = flush_aging_candidates(candidates,
                                 age_count,
                                 &aging_batches,
                                 &aging_machines);
    if (ret) {
        for (i = 0; i < age_count; i++) {
            abort_candidate_aging(&candidates[i]);
            release_scanning_candidate(&candidates[i]);
        }
        __atomic_add_fetch(&dsm_meta->cxl_reclaim.demote_conflicts,
                           age_count,
                           __ATOMIC_RELAXED);
        return ret;
    }
    /*
     * Two clocks with different jobs: the local one measures this flush's
     * latency (a delta that never leaves this machine), while the cluster one
     * is stamped into cxl_age_started_ns and compared against cxl_promoted_ns,
     * both of which other machines write and read.
     */
    completed_ns = plat_get_mono_time();
    completed_cluster_ns = dsm_cluster_time_ns();
    epoch = __atomic_add_fetch(&dsm_meta->cxl_reclaim.next_age_epoch,
                               1,
                               __ATOMIC_RELAXED);
    record_aging_stats(completed_ns - aging_start,
                       age_count,
                       aging_batches,
                       aging_machines);

    for (i = 0; i < age_count; i++) {
        enum cxl_page_observation observation;
        u8 previous_epochs;
        bool eligible;

        if (!validate_candidate_aging(&candidates[i])) {
            abort_candidate_aging(&candidates[i]);
            __atomic_add_fetch(&dsm_meta->cxl_reclaim.demote_conflicts,
                               1,
                               __ATOMIC_RELAXED);
            release_scanning_candidate(&candidates[i]);
            continue;
        }
        candidates[i].aged_mapping_count = 0;
        release_candidate_aliases(&candidates[i]);

        lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
        previous_epochs = candidates[i].page->cxl_cold_epochs;
        observation = cxl_policy_observe_locked(&candidates[i],
                                                epoch,
                                                completed_cluster_ns);
        eligible = observation == CXL_OBSERVATION_STABLE_COLD;
        if (eligible
            && (completed_cluster_ns < candidates[i].page->cxl_promoted_ns
                || completed_cluster_ns - candidates[i].page->cxl_promoted_ns
                           < CXL_PROMOTION_COOLDOWN_NS)) {
            eligible = false;
            cooldown_skips++;
        }
        unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);

        if (observation == CXL_OBSERVATION_ARMED) {
            armed++;
        } else if (observation == CXL_OBSERVATION_REREFERENCED) {
            second_chances++;
        } else if (observation == CXL_OBSERVATION_ONE_EPOCH_COLD) {
            one_epoch_cold++;
        } else if (previous_epochs < CXL_CLOCK_STABLE_COLD_EPOCHS) {
            stable_cold++;
        }

        if (eligible && allow_demote
            && cold_count < CXL_DEMOTE_RATE_BATCH
            && claim_scanning_candidate(&candidates[i])) {
            if (cold_count != i)
                candidates[cold_count] = candidates[i];
            cold_count++;
        } else {
            release_scanning_candidate(&candidates[i]);
        }
    }
    if (armed)
        __atomic_add_fetch(&dsm_meta->cxl_reclaim.clock_armed_pages,
                           armed,
                           __ATOMIC_RELAXED);
    if (second_chances) {
        u64 total = __atomic_add_fetch(
                &dsm_meta->cxl_reclaim.clock_second_chances,
                second_chances,
                __ATOMIC_RELAXED);

        __atomic_add_fetch(&dsm_meta->cxl_reclaim.clock_rereferenced,
                           second_chances,
                           __ATOMIC_RELAXED);
        report_clock_stats(total, second_chances);
    }
    if (one_epoch_cold)
        __atomic_add_fetch(&dsm_meta->cxl_reclaim.clock_one_epoch_cold,
                           one_epoch_cold,
                           __ATOMIC_RELAXED);
    if (cooldown_skips)
        __atomic_add_fetch(&dsm_meta->cxl_reclaim.clock_cooldown_skips,
                           cooldown_skips,
                           __ATOMIC_RELAXED);
    if (stable_cold)
        __atomic_add_fetch(&dsm_meta->cxl_reclaim.clock_stable_cold,
                           stable_cold,
                           __ATOMIC_RELAXED);
    count = cold_count;
    }
#else
    {
        u32 raw_count = count;

    if (!allow_demote) {
        for (i = 0; i < count; i++)
            release_scanning_candidate(&candidates[i]);
        return 0;
    }
    count = MIN(count, (u32)CXL_DEMOTE_RATE_BATCH);
    for (i = count; i < raw_count; i++)
        release_scanning_candidate(&candidates[i]);
    for (i = 0; i < count; i++) {
        ret = refresh_candidate_policy(&candidates[i]);
        if (ret || !claim_scanning_candidate(&candidates[i])) {
            release_scanning_candidate(&candidates[i]);
            if (i + 1 < count)
                candidates[i] = candidates[count - 1];
            count--;
            i--;
        }
    }
    }
#endif
    if (count == 0)
        return 0;

    lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    dsm_meta->cxl_reclaim.next_demote_ns = now + CXL_DEMOTE_INTERVAL_NS;
    unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);

    total_start = plat_get_mono_time();
    phase_start = total_start;
    for (i = 0; i < count; i++) {
        ret = prepare_candidate(&candidates[i]);
        if (ret) {
            __atomic_add_fetch(&dsm_meta->cxl_reclaim.demote_conflicts,
                               1,
                               __ATOMIC_RELAXED);
            __atomic_add_fetch(&dsm_meta->cxl_reclaim.clock_scan_skips,
                               1,
                               __ATOMIC_RELAXED);
            if (ret == -EPERM || ret == -E2BIG || ret == -ENOENT) {
                lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
                account_page_policy_locked(candidates[i].page,
                                           candidates[i].pmo,
                                           false,
                                           0);
                reset_page_aging_locked(candidates[i].page,
                                         candidates[i].mapping_generation);
                unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
            }
            defer_candidate(&candidates[i]);
            continue;
        }
        if (prepared_count != i)
            candidates[prepared_count] = candidates[i];
        prepared_count++;
    }
    count = prepared_count;
    if (count == 0)
        return -EAGAIN;
    prepare_ns = plat_get_mono_time() - phase_start;

    phase_start = plat_get_mono_time();
    ret = flush_candidates(candidates, count);
    if (ret) {
        restore_candidates(candidates, count);
        return ret;
    }
    mark_candidates_flushed(candidates, count);
    flush_ns = plat_get_mono_time() - phase_start;

    phase_start = plat_get_mono_time();
    ret = copy_candidates(candidates, count);
    if (ret) {
        restore_candidates(candidates, count);
        return ret;
    }
    copy_ns = plat_get_mono_time() - phase_start;

    /*
     * PTEs are still migration entries here, so no row access can observe the
     * cleared bit before finish_candidate() installs DRAM or removes the
     * non-owner mapping.  A profiler transport failure must not compromise the
     * memory-management operation; the final snapshot will report the stale
     * bitmap if this best-effort notification ever fails.
     */
    phase_start = plat_get_mono_time();
    if (clear_candidate_bitmaps(candidates, count) != 0)
        kwarn("[cxlprof_live] failed to clear one or more demoted mappings\n");
    bitmap_ns = plat_get_mono_time() - phase_start;

    phase_start = plat_get_mono_time();
    for (i = 0; i < count; i++)
        finish_candidate(&candidates[i]);
    finish_ns = plat_get_mono_time() - phase_start;
    record_protocol_stats(prepare_ns,
                          flush_ns,
                          copy_ns,
                          bitmap_ns,
                          finish_ns,
                          plat_get_mono_time() - total_start,
                          count);
    return count;
}

/*
 * Execute at most one bounded unit of work.  The cluster token covers both
 * deferred frees and candidate demotion, so candidates[] and all distributed
 * phases remain a true singleton even though every machine has a worker.
 */
static int cxl_reclaim_run(u32 max_pages)
{
    bool demand_pending;
    bool free_pending;
    bool scan_due = false;
    bool allow_demote = false;
    /*
     * next_scan_ns / next_demote_ns live in dsm_meta and are armed by
     * whichever machine holds the token, so this comparison crosses machine
     * boundaries and must use the cluster clock.  With plat_get_mono_time()
     * the earliest-booted machine's clock is tens of seconds ahead, it wins
     * every race, and re-arms the deadline into a future the other machines
     * never reach -- an 8-machine run had all 473 reclaim reports on
     * machine 0 and none anywhere else.
     */
    u64 now = dsm_cluster_time_ns();
    int reclaimed;

    /* Lock order is FIFO state first, then aggregate accounting. */
    lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    lock(&dsm_meta->cxl_reclaim.account_guard.lock);
    demand_pending = dsm_meta->cxl_reclaim.pending_reclaim_pages != 0;
    if (demand_pending) {
        u64 resident = dsm_meta->cxl_reclaim.resident_pages;
        u64 reserved = dsm_meta->cxl_reclaim.resident_reserved_pages;
        u64 limit = dsm_meta->cxl_reclaim.limit_pages;

        if (resident > limit || reserved > limit - resident) {
            demand_pending = true;
        } else {
            /* Usage is back at the soft threshold; retire the stale hint. */
            dsm_meta->cxl_reclaim.pending_reclaim_pages = 0;
            demand_pending = false;
        }
    }
    free_pending = dsm_meta->cxl_reclaim.free_pending_pages != 0;
    if (!demand_pending && !free_pending) {
        unlock(&dsm_meta->cxl_reclaim.account_guard.lock);
        unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
        return 0;
    }
    if (demand_pending
        && now >= dsm_meta->cxl_reclaim.next_scan_ns) {
        scan_due = true;
        allow_demote = now >= dsm_meta->cxl_reclaim.next_demote_ns;
        dsm_meta->cxl_reclaim.next_scan_ns =
                now + CXL_CLOCK_SCAN_INTERVAL_NS;
    }
    if (!free_pending && !scan_due) {
        unlock(&dsm_meta->cxl_reclaim.account_guard.lock);
        unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
        return 0;
    }
    if (dsm_meta->cxl_reclaim.reclaiming) {
        unlock(&dsm_meta->cxl_reclaim.account_guard.lock);
        unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
        return -EAGAIN;
    }
    if (dsm_meta->cxl_reclaim.snapshotting) {
        unlock(&dsm_meta->cxl_reclaim.account_guard.lock);
        unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
        return -EAGAIN;
    }
    __atomic_store_n(&dsm_meta->cxl_reclaim.reclaiming,
                     1,
                     __ATOMIC_RELEASE);
    unlock(&dsm_meta->cxl_reclaim.account_guard.lock);
    unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);

    reclaimed = free_pending ? retry_pending_frees(max_pages) : 0;
    if (reclaimed == 0 && demand_pending && scan_due)
        reclaimed = demote_one_batch(max_pages, now, allow_demote);

    lock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    __atomic_store_n(&dsm_meta->cxl_reclaim.reclaiming,
                     0,
                     __ATOMIC_RELEASE);
    unlock(&dsm_meta->cxl_reclaim.fifo_guard.lock);
    return reclaimed;
}

static void report_admission_stats(void)
{
    u64 local_fallbacks = 0;
    u64 misses;
    u64 fallbacks;
    u64 reported_misses;
    u64 reported_fallbacks;
    u64 async_requests;
    u64 reclaimed;
    u64 resident;
    u64 reserved;
    u64 limit;
    bool report_misses;
    bool report_fallbacks;
    u32 cpu;

    for (cpu = 0; cpu < PLAT_CPU_NUM; cpu++)
        local_fallbacks += __atomic_load_n(&local_fault_fallbacks[cpu],
                                           __ATOMIC_RELAXED);

    lock(&dsm_meta->cxl_reclaim.account_guard.lock);
    if (local_fallbacks > local_fault_fallbacks_published) {
        dsm_meta->cxl_reclaim.fault_fallbacks +=
                local_fallbacks - local_fault_fallbacks_published;
        local_fault_fallbacks_published = local_fallbacks;
    }
    misses = dsm_meta->cxl_reclaim.soft_limit_overcommits;
    fallbacks = dsm_meta->cxl_reclaim.fault_fallbacks;
    reported_misses = dsm_meta->cxl_reclaim.admission_reported_misses;
    reported_fallbacks =
            dsm_meta->cxl_reclaim.admission_reported_fallbacks;
    report_misses = misses != 0
                    && (reported_misses == 0
                        || misses - reported_misses
                                   >= CXL_ADMISSION_REPORT_INTERVAL);
    report_fallbacks = fallbacks != 0
                       && (reported_fallbacks == 0
                           || fallbacks - reported_fallbacks
                                      >= CXL_ADMISSION_REPORT_INTERVAL);
    if (!report_misses && !report_fallbacks) {
        unlock(&dsm_meta->cxl_reclaim.account_guard.lock);
        return;
    }
    dsm_meta->cxl_reclaim.admission_reported_misses = misses;
    dsm_meta->cxl_reclaim.admission_reported_fallbacks = fallbacks;
    async_requests = dsm_meta->cxl_reclaim.async_reclaim_requests;
    reclaimed = dsm_meta->cxl_reclaim.reclaimed_pages;
    resident = dsm_meta->cxl_reclaim.resident_pages;
    reserved = dsm_meta->cxl_reclaim.resident_reserved_pages;
    limit = dsm_meta->cxl_reclaim.limit_pages;
    unlock(&dsm_meta->cxl_reclaim.account_guard.lock);

    kinfo("[CXL_ADMISSION] soft_limit_overcommits=%lu async_requests=%lu "
          "scheduled_fallbacks=%lu promotions=%lu refaults=%lu "
          "conflicts=%lu reclaimed=%lu resident=%lu reserved=%lu "
          "limit=%lu\n",
          misses,
          async_requests,
          fallbacks,
          __atomic_load_n(&dsm_meta->cxl_reclaim.promotion_pages,
                          __ATOMIC_RELAXED),
          __atomic_load_n(&dsm_meta->cxl_reclaim.refault_pages,
                          __ATOMIC_RELAXED),
          __atomic_load_n(&dsm_meta->cxl_reclaim.demote_conflicts,
                          __ATOMIC_RELAXED),
          reclaimed,
          resident,
          reserved,
          limit);
}

int dsm_cxl_reclaim_step(u64 max_pages)
{
    u32 cpuid;
    int reclaimed;

    if (!dsm_cxl_reclaim_enabled())
        return -EOPNOTSUPP;
    if (max_pages == 0 || max_pages > CXL_DEMOTE_MAX_BATCH)
        return -EINVAL;

    /*
     * The polling service owns a separate reclaim pthread, so this executes
     * in schedulable kernel context without occupying its foreground durable
     * queue reader or doing VM-space work from an MSI interrupt.
     */
    ivshmem_process_cxl_control_messages();

    cpuid = smp_get_cpu_id();
    if (cxl_reclaim_on_cpu[cpuid])
        return -EAGAIN;
    cxl_reclaim_on_cpu[cpuid] = true;
    reclaimed = cxl_reclaim_run((u32)max_pages);
    cxl_reclaim_on_cpu[cpuid] = false;
    report_admission_stats();
    return reclaimed;
}

#endif
