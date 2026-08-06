#include <dsm/cxl_reclaim.h>
#include <dsm/dsm-single.h>

#ifdef DSM_ENABLED

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
#include <mm/shm.h>
#include <mm/vmspace.h>
#include <object/object.h>

#define CXL_PMO_MAPPING_UNINITIALIZED 0
#define CXL_PMO_MAPPING_INITIALIZING  1
#define CXL_PMO_MAPPING_READY         2

#define CXL_ORIGIN_NOT_RELEASED 0
#define CXL_ORIGIN_RELEASING    1
#define CXL_ORIGIN_RELEASED     2

#define CXL_MAX_ABANDONED_POLLING_NODES 64

struct cxl_saved_mapping {
    pte_t *pte;
    u64 old_pteval;
    mid_t machine_id;
    struct vmspace *vmspace;
    vaddr_t va;
};

struct cxl_alias {
    struct vmspace *vmspace;
    vaddr_t va;
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
    bool prepared;
    bool pmo_pinned;
    struct cxl_alias aliases[CXL_DEMOTE_MAX_ALIASES_PER_PAGE];
    struct cxl_saved_mapping mappings[CXL_DEMOTE_MAX_MAPPINGS_PER_PAGE];
};

static struct cxl_demote_candidate candidates[CXL_DEMOTE_MAX_BATCH];
static struct lock abandoned_polling_lock;
static struct dq_node *abandoned_polling_nodes[
        CXL_MAX_ABANDONED_POLLING_NODES];
static bool abandoned_polling_slots_reserved[
        CXL_MAX_ABANDONED_POLLING_NODES];

extern void remove_page_from_pmo(struct pmobject *pmo, u64 index);

static void cxl_object_get(void *opaque)
{
    struct object *object = container_of(opaque, struct object, opaque);

    atomic_fetch_add_64(&object->refcount, 1);
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
    }
    unlock(&pmo->cxl_mapping_lock);
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
        put_pmo = true;
    }
    unlock(&pmo->cxl_mapping_lock);

    if (put_pmo)
        obj_put(pmo);
}

static u64 cxl_usage_percent(u64 allocated, u64 total)
{
    if (total == 0)
        return 0;
    return allocated * 100 / total;
}

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

void dsm_cxl_reclaim_init(void)
{
    u64 total_pages = 0;
    u64 limit_pages;
    int i;

    /* These objects live in each kernel image, not in shared DSM metadata. */
    lock_init(&abandoned_polling_lock);
    memset(abandoned_polling_nodes, 0, sizeof(abandoned_polling_nodes));
    memset(abandoned_polling_slots_reserved,
           0,
           sizeof(abandoned_polling_slots_reserved));

    if (CUR_MACHINE_ID != 0)
        return;

    for (i = 0; i < cxlmem_map_num; i++)
        total_pages += global_cxl_mem[i]->pool_phys_page_num;
    limit_pages = (u64)DSM_CXL_DEMOTE_LIMIT_MB * 1024 * 1024 / PAGE_SIZE;
    limit_pages = MIN(limit_pages, total_pages);

    lock_init(&dsm_meta->cxl_reclaim.lock);
    lock_init(&dsm_meta->msi_rpc_lock);
    init_list_head(&dsm_meta->cxl_reclaim.fifo);
    dsm_meta->cxl_reclaim.total_pages = total_pages;
    dsm_meta->cxl_reclaim.limit_pages = limit_pages;
    dsm_meta->cxl_reclaim.allocated_pages = 0;
    dsm_meta->cxl_reclaim.reserved_pages = 0;
    dsm_meta->cxl_reclaim.resident_pages = 0;
    dsm_meta->cxl_reclaim.resident_reserved_pages = 0;
    dsm_meta->cxl_reclaim.reclaimed_pages = 0;
    dsm_meta->cxl_reclaim.next_sequence = 1;
    dsm_meta->cxl_reclaim.next_rpc_id = 1;
    __atomic_store_n(&dsm_meta->cxl_reclaim.reclaiming,
                     0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&dsm_meta->cxl_reclaim.initialized,
                     1,
                     __ATOMIC_RELEASE);

    kinfo("[CXL_RECLAIM] initialized total_pages=%lu limit_pages=%lu "
          "limit_mb=%d high=%d low=%d batch=%d\n",
          total_pages,
          limit_pages,
          DSM_CXL_DEMOTE_LIMIT_MB,
          DSM_CXL_DEMOTE_HIGH_WATERMARK,
          DSM_CXL_DEMOTE_LOW_WATERMARK,
          DSM_CXL_DEMOTE_BATCH_PAGES);
}

int dsm_cxl_reserve_pages(u64 pages)
{
    u64 projected;
    int reclaim_needed;

    if (!dsm_meta
        || !__atomic_load_n(&dsm_meta->cxl_reclaim.initialized,
                            __ATOMIC_ACQUIRE))
        return 0;

    lock(&dsm_meta->cxl_reclaim.lock);
    projected = dsm_meta->cxl_reclaim.allocated_pages
                + dsm_meta->cxl_reclaim.reserved_pages + pages;
    if (projected < dsm_meta->cxl_reclaim.allocated_pages
        || projected > dsm_meta->cxl_reclaim.total_pages) {
        unlock(&dsm_meta->cxl_reclaim.lock);
        return -ENOMEM;
    }
    dsm_meta->cxl_reclaim.reserved_pages += pages;
    reclaim_needed = cxl_usage_percent(
                             dsm_meta->cxl_reclaim.resident_pages,
                             dsm_meta->cxl_reclaim.limit_pages)
                     >= DSM_CXL_DEMOTE_HIGH_WATERMARK;
    unlock(&dsm_meta->cxl_reclaim.lock);
    return reclaim_needed;
}

void dsm_cxl_commit_reserved_pages(u64 pages)
{
    if (!dsm_meta
        || !__atomic_load_n(&dsm_meta->cxl_reclaim.initialized,
                            __ATOMIC_ACQUIRE))
        return;

    lock(&dsm_meta->cxl_reclaim.lock);
    BUG_ON(dsm_meta->cxl_reclaim.reserved_pages < pages);
    dsm_meta->cxl_reclaim.reserved_pages -= pages;
    dsm_meta->cxl_reclaim.allocated_pages += pages;
    unlock(&dsm_meta->cxl_reclaim.lock);
}

void dsm_cxl_cancel_reserved_pages(u64 pages)
{
    if (!dsm_meta
        || !__atomic_load_n(&dsm_meta->cxl_reclaim.initialized,
                            __ATOMIC_ACQUIRE))
        return;

    lock(&dsm_meta->cxl_reclaim.lock);
    BUG_ON(dsm_meta->cxl_reclaim.reserved_pages < pages);
    dsm_meta->cxl_reclaim.reserved_pages -= pages;
    unlock(&dsm_meta->cxl_reclaim.lock);
}

int dsm_cxl_reserve_resident_pages(u64 pages)
{
    u64 projected;
    int reclaimed;

    if (!dsm_meta
        || !__atomic_load_n(&dsm_meta->cxl_reclaim.initialized,
                            __ATOMIC_ACQUIRE))
        return 0;

    while (true) {
        lock(&dsm_meta->cxl_reclaim.lock);
        projected = dsm_meta->cxl_reclaim.resident_pages
                    + dsm_meta->cxl_reclaim.resident_reserved_pages + pages;
        if (projected >= dsm_meta->cxl_reclaim.resident_pages
            && projected <= dsm_meta->cxl_reclaim.limit_pages) {
            dsm_meta->cxl_reclaim.resident_reserved_pages += pages;
            unlock(&dsm_meta->cxl_reclaim.lock);
            return 0;
        }
        unlock(&dsm_meta->cxl_reclaim.lock);

        reclaimed = dsm_cxl_reclaim_if_needed(pages, true);
        if (reclaimed <= 0)
            return -EAGAIN;
    }
}

void dsm_cxl_cancel_resident_pages(u64 pages)
{
    if (!dsm_meta
        || !__atomic_load_n(&dsm_meta->cxl_reclaim.initialized,
                            __ATOMIC_ACQUIRE))
        return;

    lock(&dsm_meta->cxl_reclaim.lock);
    BUG_ON(dsm_meta->cxl_reclaim.resident_reserved_pages < pages);
    dsm_meta->cxl_reclaim.resident_reserved_pages -= pages;
    unlock(&dsm_meta->cxl_reclaim.lock);
}

u64 dsm_cxl_reclaimed_pages(void)
{
    u64 reclaimed = 0;

    if (!dsm_meta
        || !__atomic_load_n(&dsm_meta->cxl_reclaim.initialized,
                            __ATOMIC_ACQUIRE))
        return 0;
    lock(&dsm_meta->cxl_reclaim.lock);
    reclaimed = dsm_meta->cxl_reclaim.reclaimed_pages;
    unlock(&dsm_meta->cxl_reclaim.lock);
    return reclaimed;
}

int dsm_cxl_track_page(paddr_t cxl_pa, paddr_t origin_pa,
                       struct pmobject *pmo, u64 pmo_index,
                       struct vmspace *vmspace, vaddr_t va,
                       mid_t owner_mid)
{
    struct page *page;
    int reclaim_needed;

    if (!dsm_meta || !pmo || !vmspace
        || owner_mid < 0 || owner_mid >= CLUSTER_MACHINE_NUM)
        return -EINVAL;
    page = validated_cxl_page(cxl_pa);
    if (!page || get_paddr_machine_id(origin_pa) != owner_mid
        || get_page_from_pmo(pmo, pmo_index) != cxl_pa)
        return -EINVAL;

    lock(&dsm_meta->cxl_reclaim.lock);
    if (page->cxl_reclaim_state != CXL_RECLAIM_NONE) {
        unlock(&dsm_meta->cxl_reclaim.lock);
        return -EEXIST;
    }

    init_empty_node(&page->cxl_reclaim_node);
    page->cxl_origin_pa = origin_pa;
    page->cxl_pmo = (u64)pmo;
    page->cxl_pmo_index = pmo_index;
    page->cxl_owner_mid = owner_mid;
    page->cxl_sequence = dsm_meta->cxl_reclaim.next_sequence++;
    page->cxl_reclaim_phase = CXL_RECLAIM_PHASE_IDLE;
    page->cxl_free_requested = 0;
    page->cxl_origin_release_state = CXL_ORIGIN_NOT_RELEASED;
    page->cxl_reclaim_state = CXL_RECLAIM_RESIDENT;
    list_append(&page->cxl_reclaim_node, &dsm_meta->cxl_reclaim.fifo);
    BUG_ON(dsm_meta->cxl_reclaim.resident_reserved_pages == 0);
    dsm_meta->cxl_reclaim.resident_reserved_pages--;
    dsm_meta->cxl_reclaim.resident_pages++;
    BUG_ON(dsm_meta->cxl_reclaim.resident_pages
           + dsm_meta->cxl_reclaim.resident_reserved_pages
           > dsm_meta->cxl_reclaim.limit_pages);
    reclaim_needed = cxl_usage_percent(
                             dsm_meta->cxl_reclaim.resident_pages,
                             dsm_meta->cxl_reclaim.limit_pages)
                     >= DSM_CXL_DEMOTE_HIGH_WATERMARK;
    unlock(&dsm_meta->cxl_reclaim.lock);

    UNUSED(va);
    return reclaim_needed;
}

static bool try_pin_candidate_pmo(struct page *page, struct pmobject **pmo_out)
{
    struct pmobject *pmo = (struct pmobject *)page->cxl_pmo;
    bool pinned = false;

    if (!pmo)
        return false;
    cxl_pmo_mapping_init(pmo);
    lock(&pmo->cxl_mapping_lock);
    if (!list_empty(&pmo->cxl_mapping_list)) {
        cxl_object_get(pmo);
        pinned = true;
    }
    unlock(&pmo->cxl_mapping_lock);

    if (pinned)
        *pmo_out = pmo;
    return pinned;
}

static u32 select_candidates(u32 limit)
{
    struct list_head *head = &dsm_meta->cxl_reclaim.fifo;
    struct list_head *node;
    u32 count = 0;

    lock(&dsm_meta->cxl_reclaim.lock);
    node = head->next;
    while (node != head && count < limit) {
        struct page *page =
                list_entry(node, struct page, cxl_reclaim_node);
        struct pmobject *pmo = NULL;

        node = node->next;
        if (page->cxl_reclaim_state != CXL_RECLAIM_RESIDENT
            || !try_pin_candidate_pmo(page, &pmo))
            continue;

        page->cxl_reclaim_state = CXL_RECLAIM_DEMOTING;
        page->cxl_reclaim_phase = CXL_RECLAIM_PHASE_SELECTED;
        memset(&candidates[count], 0, sizeof(candidates[count]));
        candidates[count].page = page;
        candidates[count].pmo = pmo;
        candidates[count].pmo_pinned = true;
        candidates[count].cxl_pa = cxl_page_pa(page);
        candidates[count].origin_pa = page->cxl_origin_pa;
        candidates[count].pmo_index = page->cxl_pmo_index;
        candidates[count].owner_mid = page->cxl_owner_mid;
        count++;
    }
    unlock(&dsm_meta->cxl_reclaim.lock);
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

static int snapshot_candidate_aliases(struct cxl_demote_candidate *candidate)
{
    struct pmobject *pmo = candidate->pmo;
    struct vmregion *vmr;
    u32 count = 0;

    lock(&pmo->cxl_mapping_lock);
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
        candidate->aliases[count].read_locked = false;
        cxl_object_get(candidate->aliases[count].vmspace);
        count++;
    }
    unlock(&pmo->cxl_mapping_lock);
    candidate->alias_count = count;
    return 0;
}

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

static int prepare_candidate(struct cxl_demote_candidate *candidate)
{
    u32 alias_idx;
    int ret;

    ret = snapshot_candidate_aliases(candidate);
    if (ret)
        return ret;
    if (candidate->alias_count == 0)
        return -ENOENT;

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
            set_migration_entry(pte);
        }
        unlock(&alias->vmspace->pgtbl_lock);
    }

    candidate->prepared = true;
    lock(&dsm_meta->cxl_reclaim.lock);
    if (candidate->page->cxl_reclaim_state == CXL_RECLAIM_DEMOTING)
        candidate->page->cxl_reclaim_phase = CXL_RECLAIM_PHASE_PREPARED;
    unlock(&dsm_meta->cxl_reclaim.lock);
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

    lock(&dsm_meta->cxl_reclaim.lock);
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
    }
out:
    unlock(&dsm_meta->cxl_reclaim.lock);
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
            cxl_object_get(vmspace);
            break;
        }
    }
    unlock(&pmo->cxl_mapping_lock);
    return vmspace;
}

int dsm_cxl_handle_batch(struct cxl_demote_batch_op *ops, u64 ops_count,
                         u64 phase)
{
    struct tlb_flush_batch_op tlb_ops[CXL_DEMOTE_MAX_BATCH];
    struct vmspace *vmspaces[CXL_DEMOTE_MAX_BATCH] = { 0 };
    struct page *pages[CXL_DEMOTE_MAX_BATCH] = { 0 };
    u64 i;
    int ret = 0;

    if (!ops || ops_count == 0 || ops_count > CXL_DEMOTE_MAX_BATCH)
        return -EINVAL;
    if (phase > CXL_DEMOTE_PHASE_FREE_ORIGIN)
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

        if (phase == CXL_DEMOTE_PHASE_FLUSH) {
            void *pgtbl;
            paddr_t pa = 0;
            pte_t *pte = NULL;

            vmspaces[i] = validated_flush_vmspace(pages[i], &ops[i]);
            if (!vmspaces[i]) {
                ret = -EPERM;
                goto out;
            }
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

/* abandoned_polling_lock must be held. */
static void reap_abandoned_polling_nodes(void)
{
    u32 slot;

    for (slot = 0; slot < CXL_MAX_ABANDONED_POLLING_NODES; slot++) {
        struct dq_node *node = abandoned_polling_nodes[slot];
        s32 status;

        if (!node)
            continue;
        status = atomic_load_32(&node->status);
        if (status == DQ_DONE || status == DQ_CRASH) {
            dq_mark_consumed(node);
            abandoned_polling_nodes[slot] = NULL;
        }
    }
}

static int reserve_abandoned_polling_slot(void)
{
    u32 slot;

    lock(&abandoned_polling_lock);
    reap_abandoned_polling_nodes();
    for (slot = 0; slot < CXL_MAX_ABANDONED_POLLING_NODES; slot++) {
        if (!abandoned_polling_nodes[slot]
            && !abandoned_polling_slots_reserved[slot]) {
            abandoned_polling_slots_reserved[slot] = true;
            unlock(&abandoned_polling_lock);
            return slot;
        }
    }
    unlock(&abandoned_polling_lock);
    return -EAGAIN;
}

static void release_abandoned_polling_slot(u32 slot, struct dq_node *node)
{
    BUG_ON(slot >= CXL_MAX_ABANDONED_POLLING_NODES);
    lock(&abandoned_polling_lock);
    BUG_ON(!abandoned_polling_slots_reserved[slot]);
    BUG_ON(abandoned_polling_nodes[slot]);
    abandoned_polling_nodes[slot] = node;
    abandoned_polling_slots_reserved[slot] = false;
    unlock(&abandoned_polling_lock);
}

static int send_polling_batch(mid_t target_mid,
                              struct cxl_demote_batch_op *ops,
                              u32 count, u32 phase)
{
    struct polling_shm_region *target_shm;
    struct polling_request request;
    struct dq_node *msg;
    u32 i;
    int abandoned_slot;
    int ret;

    target_shm = (struct polling_shm_region *)
            dsm_meta->shm_data[target_mid].data;
    if (!target_shm)
        return -EINVAL;

    /*
     * Page destruction and pending-free retries are independent of the
     * cluster-wide reclaimer, so multiple CPUs can enter this path at once.
     * Reserve an abandoned-node slot before publishing the request: if the
     * bounded wait expires, the queue node cannot be recycled until a later
     * caller observes DONE/CRASH and consumes it.  The slot table has its own
     * short critical section; never hold the global MSI RPC lock across a
     * polling round trip.  The request itself stays on this call's stack so
     * another producer cannot overwrite it before dq_enqueue() copies it.
     */
    abandoned_slot = reserve_abandoned_polling_slot();
    if (abandoned_slot < 0)
        return abandoned_slot;

    memset(&request, 0, sizeof(request));
    request.type = POLLING_KERNEL_REQ_CXL_DEMOTE_BATCH;
    request.cxl_demote.phase = phase;
    request.cxl_demote.count = count;
    for (i = 0; i < count; i++) {
        request.cxl_demote.ops[i].src_pa = ops[i].src_pa;
        request.cxl_demote.ops[i].dst_pa = ops[i].dst_pa;
        request.cxl_demote.ops[i].fault_va = ops[i].fault_va;
        request.cxl_demote.ops[i].vmspace_ptr = ops[i].vmspace_ptr;
        request.cxl_demote.ops[i].txn_id = ops[i].txn_id;
    }

    msg = dq_alloc_node_timeout(target_shm, DSM_CXL_RPC_TIMEOUT_NS);
    if (!msg) {
        release_abandoned_polling_slot(abandoned_slot, NULL);
        return -ETIMEDOUT;
    }
    dq_enqueue(target_shm, msg, &request);
    ret = dq_wait_for_done_timeout(msg, DSM_CXL_RPC_TIMEOUT_NS);
    if (ret) {
        release_abandoned_polling_slot(abandoned_slot, msg);
        return ret;
    }
    ret = msg->resp.cxl_demote.result;
    dq_mark_consumed(msg);
    release_abandoned_polling_slot(abandoned_slot, NULL);
    return ret;
}

static void clear_msi_request_if_matching(mid_t target_mid, u64 rpc_id)
{
    lock(&dsm_meta->msi_test_msg[target_mid].msg_lock);
    if (dsm_meta->msi_test_msg[target_mid].cxl_batch_rpc_id == rpc_id
        && dsm_meta->msi_test_msg[target_mid].msg_from == CUR_MACHINE_ID) {
        dsm_meta->msi_test_msg[target_mid].msg_type = 0;
        dsm_meta->msi_test_msg[target_mid].msg_from = 0xFFFFFFFF;
    }
    unlock(&dsm_meta->msi_test_msg[target_mid].msg_lock);
}

static int send_msi_batch(mid_t target_mid,
                          struct cxl_demote_batch_op *ops,
                          u32 count, u32 phase)
{
    mid_t my_id = CUR_MACHINE_ID;
    u64 rpc_id = __atomic_add_fetch(&dsm_meta->cxl_reclaim.next_rpc_id,
                                    1,
                                    __ATOMIC_ACQ_REL);
    u64 deadline;
    u32 i;

    lock(&dsm_meta->msi_rpc_lock);
    lock(&dsm_meta->msi_test_msg[target_mid].msg_lock);
    dsm_meta->msi_test_msg[target_mid].msg_from = my_id;
    dsm_meta->msi_test_msg[target_mid].msg_type =
            MSI_MSG_TYPE_CXL_DEMOTE_BATCH;
    dsm_meta->msi_test_msg[target_mid].reply_received = 0;
    dsm_meta->msi_test_msg[target_mid].cxl_batch_phase = phase;
    dsm_meta->msi_test_msg[target_mid].cxl_batch_count = count;
    dsm_meta->msi_test_msg[target_mid].cxl_batch_rpc_id = rpc_id;
    for (i = 0; i < count; i++) {
        dsm_meta->msi_test_msg[target_mid].cxl_batch_ops[i].src_pa =
                ops[i].src_pa;
        dsm_meta->msi_test_msg[target_mid].cxl_batch_ops[i].dst_pa =
                ops[i].dst_pa;
        dsm_meta->msi_test_msg[target_mid].cxl_batch_ops[i].fault_va =
                ops[i].fault_va;
        dsm_meta->msi_test_msg[target_mid].cxl_batch_ops[i].vmspace_ptr =
                ops[i].vmspace_ptr;
        dsm_meta->msi_test_msg[target_mid].cxl_batch_ops[i].txn_id =
                ops[i].txn_id;
    }
    unlock(&dsm_meta->msi_test_msg[target_mid].msg_lock);

    lock(&dsm_meta->msi_test_msg[my_id].msg_lock);
    dsm_meta->msi_test_msg[my_id].reply_received = 0;
    dsm_meta->msi_test_msg[my_id].reply_from = 0xFFFFFFFF;
    dsm_meta->msi_test_msg[my_id].cxl_batch_reply_rpc_id = 0;
    unlock(&dsm_meta->msi_test_msg[my_id].msg_lock);

    if (ivshmem_send_msi(target_mid, 0) != 0) {
        clear_msi_request_if_matching(target_mid, rpc_id);
        unlock(&dsm_meta->msi_rpc_lock);
        return -EIO;
    }

    deadline = plat_get_mono_time() + DSM_CXL_RPC_TIMEOUT_NS;
    while (plat_get_mono_time() < deadline) {
        u32 received, from;
        u64 reply_rpc_id;
        int result;
        extern void handle_ipi(void);

        lock(&dsm_meta->msi_test_msg[my_id].msg_lock);
        received = dsm_meta->msi_test_msg[my_id].reply_received;
        from = dsm_meta->msi_test_msg[my_id].reply_from;
        reply_rpc_id =
                dsm_meta->msi_test_msg[my_id].cxl_batch_reply_rpc_id;
        result = dsm_meta->msi_test_msg[my_id].cxl_batch_result;
        unlock(&dsm_meta->msi_test_msg[my_id].msg_lock);
        if (received && from == target_mid && reply_rpc_id == rpc_id) {
            unlock(&dsm_meta->msi_rpc_lock);
            return result;
        }
        handle_ipi();
        CPU_PAUSE();
    }

    clear_msi_request_if_matching(target_mid, rpc_id);
    unlock(&dsm_meta->msi_rpc_lock);
    return -ETIMEDOUT;
}

static int send_remote_batch(mid_t target_mid,
                             struct cxl_demote_batch_op *ops,
                             u32 count, u32 phase)
{
    if (target_mid == CUR_MACHINE_ID)
        return dsm_cxl_handle_batch(ops, count, phase);
    if (ivshmem_get_msg_mode() == IVSHMEM_MSG_MODE_MSI)
        return send_msi_batch(target_mid, ops, count, phase);
    return send_polling_batch(target_mid, ops, count, phase);
}

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

    lock(&dsm_meta->cxl_reclaim.lock);
    for (i = 0; i < count; i++) {
        if (batch[i].page->cxl_reclaim_state == CXL_RECLAIM_DEMOTING
            && batch[i].page->cxl_reclaim_phase
                       == CXL_RECLAIM_PHASE_PREPARED)
            batch[i].page->cxl_reclaim_phase = CXL_RECLAIM_PHASE_FLUSHED;
    }
    unlock(&dsm_meta->cxl_reclaim.lock);
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

static void finalize_cxl_page_free(struct page *page)
{
    lock(&dsm_meta->cxl_reclaim.lock);
    if ((page->cxl_reclaim_state != CXL_RECLAIM_FREE_PENDING
         && page->cxl_reclaim_state != CXL_RECLAIM_FREEING)
        || __atomic_load_n(&page->cxl_origin_release_state, __ATOMIC_ACQUIRE)
                   != CXL_ORIGIN_RELEASED) {
        unlock(&dsm_meta->cxl_reclaim.lock);
        return;
    }

    list_del(&page->cxl_reclaim_node);
    page->cxl_reclaim_state = CXL_RECLAIM_NONE;
    page->cxl_reclaim_phase = CXL_RECLAIM_PHASE_IDLE;
    page->cxl_origin_pa = 0;
    page->cxl_pmo = 0;
    page->cxl_pmo_index = 0;
    page->cxl_free_requested = 0;
    BUG_ON(dsm_meta->cxl_reclaim.allocated_pages == 0);
    BUG_ON(dsm_meta->cxl_reclaim.resident_pages == 0);
    dsm_meta->cxl_reclaim.allocated_pages--;
    dsm_meta->cxl_reclaim.resident_pages--;
    buddy_free_pages(page->pool, page);
    unlock(&dsm_meta->cxl_reclaim.lock);
}

static void defer_origin_release(struct page *page)
{
    lock(&dsm_meta->cxl_reclaim.lock);
    if (page->cxl_reclaim_state == CXL_RECLAIM_FREEING) {
        page->cxl_reclaim_state = CXL_RECLAIM_FREE_PENDING;
        list_del(&page->cxl_reclaim_node);
        list_append(&page->cxl_reclaim_node, &dsm_meta->cxl_reclaim.fifo);
    }
    unlock(&dsm_meta->cxl_reclaim.lock);
}

static int release_origin_page(struct page *page)
{
    struct cxl_demote_batch_op op;
    mid_t owner_mid;
    int ret;

    lock(&dsm_meta->cxl_reclaim.lock);
    if (page->cxl_reclaim_state != CXL_RECLAIM_FREE_PENDING
        && page->cxl_reclaim_state != CXL_RECLAIM_FREEING) {
        unlock(&dsm_meta->cxl_reclaim.lock);
        return -EINVAL;
    }
    page->cxl_reclaim_state = CXL_RECLAIM_FREEING;
    op.src_pa = cxl_page_pa(page);
    op.dst_pa = page->cxl_origin_pa;
    op.fault_va = 0;
    op.vmspace_ptr = 0;
    op.txn_id = page->cxl_sequence;
    owner_mid = page->cxl_owner_mid;
    unlock(&dsm_meta->cxl_reclaim.lock);

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

    lock(&dsm_meta->cxl_reclaim.lock);
    if (!page_check_flag(page, PG_allocated)) {
        unlock(&dsm_meta->cxl_reclaim.lock);
        return;
    }
    if (page->cxl_reclaim_state == CXL_RECLAIM_DEMOTING) {
        page->cxl_free_requested = 1;
        unlock(&dsm_meta->cxl_reclaim.lock);
        return;
    }
    if (page->cxl_reclaim_state == CXL_RECLAIM_FREE_PENDING
        || page->cxl_reclaim_state == CXL_RECLAIM_FREEING) {
        unlock(&dsm_meta->cxl_reclaim.lock);
        return;
    }
    if (page->cxl_reclaim_state == CXL_RECLAIM_RESIDENT) {
        page->cxl_reclaim_state = CXL_RECLAIM_FREEING;
        page->cxl_reclaim_phase = CXL_RECLAIM_PHASE_IDLE;
        page->cxl_pmo = 0;
        unlock(&dsm_meta->cxl_reclaim.lock);
        if (release_origin_page(page) < 0)
            kwarn("[CXL_RECLAIM] deferred origin release pa=0x%lx\n",
                  page->cxl_origin_pa);
        return;
    }

    BUG_ON(dsm_meta->cxl_reclaim.allocated_pages < pages);
    dsm_meta->cxl_reclaim.allocated_pages -= pages;
    buddy_free_pages(page->pool, page);
    unlock(&dsm_meta->cxl_reclaim.lock);
}

static int retry_pending_frees(u32 limit)
{
    struct page *pages[CXL_DEMOTE_MAX_BATCH];
    struct cxl_demote_batch_op ops[CXL_DEMOTE_MAX_BATCH];
    u32 page_count = 0;
    int freed = 0;
    mid_t mid;

    limit = MIN(limit, CXL_DEMOTE_MAX_BATCH);
    lock(&dsm_meta->cxl_reclaim.lock);
    {
        struct list_head *head = &dsm_meta->cxl_reclaim.fifo;
        struct list_head *node;

        for (node = head->next; node != head; node = node->next) {
            struct page *candidate =
                    list_entry(node, struct page, cxl_reclaim_node);

            if (candidate->cxl_reclaim_state == CXL_RECLAIM_FREE_PENDING) {
                candidate->cxl_reclaim_state = CXL_RECLAIM_FREEING;
                pages[page_count++] = candidate;
                if (page_count == limit)
                    break;
            }
        }
    }
    unlock(&dsm_meta->cxl_reclaim.lock);

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
        if (batch[i].prepared)
            restore_candidate_ptes(&batch[i]);
        release_candidate_aliases(&batch[i]);
        lock(&dsm_meta->cxl_reclaim.lock);
        if (batch[i].page->cxl_reclaim_state == CXL_RECLAIM_DEMOTING) {
            batch[i].page->cxl_reclaim_state = CXL_RECLAIM_RESIDENT;
            batch[i].page->cxl_reclaim_phase = CXL_RECLAIM_PHASE_IDLE;
        }
        unlock(&dsm_meta->cxl_reclaim.lock);
        release_candidate_pmo(&batch[i]);
    }
}

static void finish_candidate(struct cxl_demote_candidate *candidate)
{
    bool free_requested;
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

    lock(&dsm_meta->cxl_reclaim.lock);
    BUG_ON(candidate->page->cxl_reclaim_state != CXL_RECLAIM_DEMOTING);
    if (free_requested) {
        candidate->page->cxl_reclaim_state = CXL_RECLAIM_FREEING;
        candidate->page->cxl_reclaim_phase = CXL_RECLAIM_PHASE_IDLE;
        candidate->page->cxl_pmo = 0;
    } else {
        list_del(&candidate->page->cxl_reclaim_node);
        candidate->page->cxl_reclaim_state = CXL_RECLAIM_NONE;
        candidate->page->cxl_reclaim_phase = CXL_RECLAIM_PHASE_IDLE;
        candidate->page->cxl_origin_pa = 0;
        candidate->page->cxl_pmo = 0;
        candidate->page->cxl_pmo_index = 0;
        candidate->page->cxl_free_requested = 0;
        BUG_ON(dsm_meta->cxl_reclaim.allocated_pages == 0);
        BUG_ON(dsm_meta->cxl_reclaim.resident_pages == 0);
        dsm_meta->cxl_reclaim.allocated_pages--;
        dsm_meta->cxl_reclaim.resident_pages--;
        dsm_meta->cxl_reclaim.reclaimed_pages++;
        buddy_free_pages(candidate->page->pool, candidate->page);
    }
    unlock(&dsm_meta->cxl_reclaim.lock);

    release_candidate_pmo(candidate);
    if (free_requested && release_origin_page(candidate->page) < 0)
        kwarn("[CXL_RECLAIM] deferred raced free pa=0x%lx\n",
              candidate->origin_pa);
}

static void defer_candidate(struct cxl_demote_candidate *candidate)
{
    release_candidate_aliases(candidate);
    lock(&dsm_meta->cxl_reclaim.lock);
    if (candidate->page->cxl_reclaim_state == CXL_RECLAIM_DEMOTING) {
        candidate->page->cxl_reclaim_state = CXL_RECLAIM_RESIDENT;
        candidate->page->cxl_reclaim_phase = CXL_RECLAIM_PHASE_IDLE;
        list_del(&candidate->page->cxl_reclaim_node);
        list_append(&candidate->page->cxl_reclaim_node,
                    &dsm_meta->cxl_reclaim.fifo);
    }
    unlock(&dsm_meta->cxl_reclaim.lock);
    release_candidate_pmo(candidate);
}

static int demote_one_batch(u32 limit, u32 *examined)
{
    u32 count, i, prepared_count = 0;
    int ret;

    *examined = 0;
    count = select_candidates(limit);
    if (count == 0)
        return 0;
    *examined = count;

    for (i = 0; i < count; i++) {
        ret = prepare_candidate(&candidates[i]);
        if (ret) {
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

    ret = flush_candidates(candidates, count);
    if (ret) {
        restore_candidates(candidates, count);
        return ret;
    }
    mark_candidates_flushed(candidates, count);
    ret = copy_candidates(candidates, count);
    if (ret) {
        restore_candidates(candidates, count);
        return ret;
    }

    for (i = 0; i < count; i++)
        finish_candidate(&candidates[i]);
    return count;
}

int dsm_cxl_reclaim_if_needed(u64 requested_pages, bool force)
{
    u64 resident, limit, low_target, deadline, scanned, scan_limit;
    bool reclaim_to_low = false;
    bool target_initialized = false;
    u32 examined;
    int reclaimed;
    int ret;

    if (!dsm_meta
        || !__atomic_load_n(&dsm_meta->cxl_reclaim.initialized,
                            __ATOMIC_ACQUIRE))
        return 0;

    reclaimed = retry_pending_frees(DSM_CXL_DEMOTE_BATCH_PAGES);

retry:
    lock(&dsm_meta->cxl_reclaim.lock);
    limit = dsm_meta->cxl_reclaim.limit_pages;
    resident = dsm_meta->cxl_reclaim.resident_pages;
    low_target = limit * DSM_CXL_DEMOTE_LOW_WATERMARK / 100;
    if (!target_initialized) {
        /*
         * A forced request which starts above the low watermark must create
         * useful headroom, not merely free requested_pages.  In particular,
         * hard-cap page faults request one page at a time; stopping after the
         * first 64-page batch makes every waiter start another expensive
         * distributed reclaim pass.  Keep this initial target stable across
         * retries so a waiter can observe that the active reclaimer already
         * reached it and return without over-reclaiming.
         */
        reclaim_to_low = force && resident > low_target;
        target_initialized = true;
    }
    if (force
        && ((reclaim_to_low && resident <= low_target)
            || (!reclaim_to_low
                && reclaimed >= (int)requested_pages))) {
        unlock(&dsm_meta->cxl_reclaim.lock);
        return reclaimed;
    }
    if (!force
        && cxl_usage_percent(resident, limit)
                   < DSM_CXL_DEMOTE_HIGH_WATERMARK) {
        unlock(&dsm_meta->cxl_reclaim.lock);
        return reclaimed;
    }
    if (dsm_meta->cxl_reclaim.reclaiming) {
        unlock(&dsm_meta->cxl_reclaim.lock);
        /*
         * A normal reclaim request comes from the page-fault path after the
         * migrated page has already been installed and tracked.  Another CPU
         * is already reducing the shared resident set, so blocking this fault
         * only serializes otherwise independent migrations across the whole
         * cluster.  Forced reclaim is different: its caller needs a specific
         * number of pages and therefore must wait for the current pass before
         * checking the target again.
         */
        if (!force)
            return reclaimed;
        deadline = plat_get_mono_time() + DSM_CXL_RPC_TIMEOUT_NS;
        while (__atomic_load_n(&dsm_meta->cxl_reclaim.reclaiming,
                               __ATOMIC_ACQUIRE)) {
            extern void handle_ipi(void);

            if (plat_get_mono_time() >= deadline)
                return reclaimed;
            handle_ipi();
            CPU_PAUSE();
        }
        goto retry;
    }
    __atomic_store_n(&dsm_meta->cxl_reclaim.reclaiming,
                     1,
                     __ATOMIC_RELEASE);
    unlock(&dsm_meta->cxl_reclaim.lock);

    scanned = 0;
    scan_limit = resident;
    do {
        ret = demote_one_batch(DSM_CXL_DEMOTE_BATCH_PAGES, &examined);
        if (ret == -EAGAIN) {
            /*
             * A busy/migrating alias only makes this batch temporarily
             * unsuitable.  Its pages were moved to the FIFO tail, so continue
             * with the next batch instead of returning to the same fault and
             * hot-spinning.  Stop after one bounded pass if every resident
             * page was unsuitable.
             */
            scanned += examined;
            if (examined == 0 || scanned >= scan_limit) {
                kwarn_once(
                        "[CXL_RECLAIM] scanned %lu resident pages without a "
                        "reclaimable candidate\n",
                        scanned);
                break;
            }
            continue;
        }
        if (ret <= 0)
            break;
        reclaimed += ret;
        scanned = 0;
        lock(&dsm_meta->cxl_reclaim.lock);
        resident = dsm_meta->cxl_reclaim.resident_pages;
        unlock(&dsm_meta->cxl_reclaim.lock);
        scan_limit = resident;
        if (force && !reclaim_to_low
            && reclaimed >= (int)requested_pages)
            break;
    } while (resident > low_target);

    lock(&dsm_meta->cxl_reclaim.lock);
    __atomic_store_n(&dsm_meta->cxl_reclaim.reclaiming,
                     0,
                     __ATOMIC_RELEASE);
    resident = dsm_meta->cxl_reclaim.resident_pages;
    unlock(&dsm_meta->cxl_reclaim.lock);

    if (reclaimed)
        kinfo("[CXL_RECLAIM] reclaimed=%d usage=%lu%% force=%d\n",
              reclaimed,
              cxl_usage_percent(resident, limit),
              force);
    return reclaimed;
}

#endif
