#pragma once

#include <common/types.h>

struct page;
struct pmobject;
struct vmregion;
struct vmspace;

#ifndef CLUSTER_MAX_MACHINE_NUM
#define CLUSTER_MAX_MACHINE_NUM 8
#endif

#ifndef DSM_CXL_DEMOTE_LIMIT_MB
#define DSM_CXL_DEMOTE_LIMIT_MB 1024
#endif

#define CXL_DEMOTE_MAX_BATCH 64
#define CXL_DEMOTE_MAX_ALIASES_PER_PAGE 16
#define CXL_DEMOTE_MAX_MAPPINGS_PER_PAGE \
    (CXL_DEMOTE_MAX_ALIASES_PER_PAGE * CLUSTER_MAX_MACHINE_NUM)

#ifndef DSM_CXL_RPC_TIMEOUT_NS
#define DSM_CXL_RPC_TIMEOUT_NS 1000000000ULL
#endif

enum cxl_reclaim_page_state {
    CXL_RECLAIM_NONE = 0,
    CXL_RECLAIM_RESIDENT,
    CXL_RECLAIM_DEMOTING,
    CXL_RECLAIM_FREE_PENDING,
    CXL_RECLAIM_FREEING,
};

enum cxl_reclaim_page_phase {
    CXL_RECLAIM_PHASE_IDLE = 0,
    CXL_RECLAIM_PHASE_SELECTED,
    CXL_RECLAIM_PHASE_PREPARED,
    CXL_RECLAIM_PHASE_FLUSHED,
};

enum cxl_demote_batch_phase {
    CXL_DEMOTE_PHASE_FLUSH = 0,
    CXL_DEMOTE_PHASE_COPY,
    CXL_DEMOTE_PHASE_FREE_ORIGIN,
    CXL_DEMOTE_PHASE_BITMAP_DRAM,
};

enum cxl_control_transaction {
    CXL_CONTROL_TXN_DEMOTE = 0,
    CXL_CONTROL_TXN_AGING,
};

struct cxl_demote_batch_op {
    u64 src_pa;
    u64 dst_pa;
    u64 fault_va;
    u64 vmspace_ptr;
    u64 txn_id;
};

struct cxl_track_op {
    paddr_t cxl_pa;
    paddr_t origin_pa;
    struct pmobject *pmo;
    u64 pmo_index;
    struct vmspace *vmspace;
    vaddr_t va;
    u64 perm;
    mid_t owner_mid;
    bool speculative;
    int result;
};

void dsm_cxl_reclaim_init(void);
bool dsm_cxl_reclaim_enabled(void);
/*
 * True when @pa must not be installed in a page table right now because the
 * demoter has already snapshotted the mappings of that page.  Callers hold
 * vmspace->pgtbl_lock and should drop it and schedule a later fault retry.
 */
bool dsm_cxl_mapping_in_transition(struct pmobject *pmo, u64 pmo_index,
                                   paddr_t pa);
int dsm_cxl_reserve_pages(u64 pages);
/*
 * Read the cluster-wide CXL page accounting.  Diagnostics only: the three
 * values are sampled under the accounting lock but are stale by the time the
 * caller sees them.  Any pointer may be NULL.
 */
void dsm_cxl_account_snapshot(u64 *allocated, u64 *reserved, u64 *total);
void dsm_cxl_commit_reserved_pages(u64 pages);
void dsm_cxl_cancel_reserved_pages(u64 pages);
/*
 * Reserve residency without waiting for reclaim.  The configured limit is a
 * soft asynchronous-reclaim threshold; crossing it publishes coalesced demand
 * but does not reject the promotion.
 */
int dsm_cxl_reserve_resident_pages(u64 pages);
void dsm_cxl_cancel_resident_pages(u64 pages);
void dsm_cxl_note_fault_fallback(void);
int dsm_cxl_snapshot_begin(void);
void dsm_cxl_snapshot_end(void);
void dsm_cxl_free_page(struct page *page);
int dsm_cxl_reclaim_step(u64 max_pages);
u64 dsm_cxl_reclaimed_pages(void);
int dsm_cxl_track_page(paddr_t cxl_pa, paddr_t origin_pa,
                       struct pmobject *pmo, u64 pmo_index,
                       struct vmspace *vmspace, vaddr_t va,
                       u64 perm, mid_t owner_mid);
int dsm_cxl_track_pages(struct cxl_track_op *ops, u64 count);
int dsm_cxl_handle_batch(struct cxl_demote_batch_op *ops, u64 ops_count,
                         u64 phase);
int dsm_cxl_handle_aging_batch(struct cxl_demote_batch_op *ops,
                               u64 ops_count);
void dsm_cxl_vmr_init(struct vmregion *vmr);
void dsm_cxl_link_vmr(struct vmregion *vmr);
void dsm_cxl_unlink_vmr(struct vmregion *vmr);
void dsm_cxl_note_vmr_perm_change(struct vmregion *vmr);
