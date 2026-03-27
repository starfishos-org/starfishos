/*
 * dsm_ref.c — Per-machine redo-log + era partial-failure-resilient
 *             reference counting for CXL SHM objects.
 *
 * Failure model: whole-machine failure (all CPUs fail together).
 * No per-thread tracking, no TBR/RootRef, no era matrix larger than N×N
 * where N = number of machines (≤ 8).
 *
 * Redo-log protocol (verbatim from CXL-SHM §4, machine-granular):
 *
 *   link / unlink:
 *     1. Read old ref_info → extract (lcid, cnt, saw_era).
 *     2. Update era[mid][lcid] if saw_era is newer (help recovery).
 *     3. Write redo log entry {op, cnt, cur_era, ref_loc_off, refed_off}
 *        into CXL SHM with a store-fence (visible before the CAS).
 *     4. CAS ref_info: old → pack(mid, cnt±1, cur_era).
 *     5. Write *ref_loc = target_off  (or 0 for unlink).
 *     6. Increment own era.
 *
 *   recovery (dsm_ref_recover_machine):
 *     Find the highest-era redo entry of the dead machine.
 *     Determine whether its CAS committed (era-based check, see below).
 *     If committed: re-apply the pointer write (step 5 may have been lost).
 *     If not committed: nothing to do (tree state was never changed).
 */

#ifdef DSM_ENABLED

#include <dsm/dsm_ref.h>
#include <dsm/dsm-single.h>
#include <arch/sync.h>
#include <common/kprint.h>
#include <common/util.h>  /* memset */
#include <mm/mm.h>        /* phys_to_virt */

/* ======================================================================== */
/* Internal helpers                                                          */
/* ======================================================================== */

static inline void *shm_base(void)
{
    return (void *)phys_to_virt(dsm_meta->shm_paddr);
}

static inline dsm_ref_meta_t *rmeta(void)
{
    return &dsm_meta->ref_meta;
}

/* ---- offset <-> kernel virtual address ---- */

static inline u64 ptr_to_off(const void *ptr)
{
    return (u64)((const char *)ptr - (const char *)shm_base());
}

static inline void *off_to_ptr(u64 off)
{
    return (void *)((char *)shm_base() + off);
}

/* ---- era matrix ---- */

static inline u32 era_get(mid_t i, mid_t j)
{
    return (u32)atomic_load_32((s32 *)&rmeta()->era[i][j]);
}

/*
 * era_set: update era[i][j] = val and issue a store fence so the write is
 * visible to other machines before any subsequent CAS.
 */
static inline void era_set(mid_t i, mid_t j, u32 val)
{
    atomic_store_32((s32 *)&rmeta()->era[i][j], (s32)val);
    smp_mb();
}

static inline u32 era_self(void)
{
    mid_t m = CUR_MACHINE_ID;
    return era_get(m, m);
}

static inline void era_self_inc(void)
{
    mid_t m = CUR_MACHINE_ID;
    atomic_fetch_add_32((s32 *)&rmeta()->era[m][m], 1);
}

/* ---- redo log ---- */

/*
 * write_redo: write one redo log entry for the current machine.
 *
 * Slot selection mirrors CXL-SHM: (cur_era & (CNT-1)).  Because cur_era is
 * monotonically increasing per machine, each new op overwrites the oldest
 * slot — recovery always picks the entry with the maximum cur_era.
 *
 * The store fence after the write ensures the redo entry is durably visible
 * before the CAS on ref_info.
 */
static void write_redo(u16 func_id, u16 saved_cnt, u32 cur_era,
                       u64 ref_loc_off, u64 refed_off)
{
    mid_t mid  = CUR_MACHINE_ID;
    u32   slot = cur_era & (DSM_REF_REDO_CNT - 1);
    struct dsm_ref_redo *r = &rmeta()->machine[mid].redo[slot];

    r->func_id     = func_id;
    r->saved_cnt   = saved_cnt;
    r->cur_era     = cur_era;
    r->ref_loc_off = ref_loc_off;
    r->refed_off   = refed_off;

    smp_mb(); /* redo must land before the CAS below */
}

/*
 * find_latest_redo: scan all DSM_REF_REDO_CNT slots for machine `mid`
 * and return the one with the highest cur_era (= the most recent op).
 * Returns NULL if all slots are empty (func_id == 0).
 */
static struct dsm_ref_redo *find_latest_redo(mid_t mid)
{
    struct dsm_ref_redo *best = NULL;
    int i;

    for (i = 0; i < DSM_REF_REDO_CNT; i++) {
        struct dsm_ref_redo *r = &rmeta()->machine[mid].redo[i];
        if (r->func_id == 0)
            continue;
        if (!best || r->cur_era > best->cur_era)
            best = r;
    }
    return best;
}

/* ======================================================================== */
/* Public API                                                                */
/* ======================================================================== */

void dsm_ref_global_init(void)
{
    memset(rmeta(), 0, sizeof(dsm_ref_meta_t));
    dsm_info("[DSM_REF] global init done (meta @ %p, %zu bytes)\n",
             rmeta(), sizeof(dsm_ref_meta_t));
}

void dsm_ref_machine_join(void)
{
    mid_t mid = CUR_MACHINE_ID;

    /* Clear any redo state left by a previous boot of this machine. */
    memset(rmeta()->machine[mid].redo, 0,
           DSM_REF_REDO_CNT * sizeof(struct dsm_ref_redo));

    /* Publish liveness token. */
    atomic_store_64((s64 *)&rmeta()->machine[mid].mid_token, (s64)(mid + 1));
    smp_mb();

    /* Own era starts at 1 (0 means "never written", used by recovery). */
    atomic_store_32((s32 *)&rmeta()->era[mid][mid], 1);

    dsm_info("[DSM_REF] machine %d joined (token=%llu)\n",
             mid, (unsigned long long)(mid + 1));
}

void dsm_ref_machine_leave(void)
{
    mid_t mid = CUR_MACHINE_ID;

    /* Normal exit: clear redo (nothing is in-flight at shutdown). */
    memset(rmeta()->machine[mid].redo, 0,
           DSM_REF_REDO_CNT * sizeof(struct dsm_ref_redo));

    atomic_store_64((s64 *)&rmeta()->machine[mid].mid_token, 0);
    smp_mb();

    dsm_info("[DSM_REF] machine %d left\n", mid);
}

/*
 * dsm_ref_link — increment ref_cnt of object at `target_off`, write pointer.
 *
 * The CAS loop + redo-log write is the hot path.  The critical ordering is:
 *   redo write  →  smp_mb  →  CAS on ref_info  →  *ref_loc write  →  era++
 *
 * A crash anywhere in this sequence leaves enough information for recovery.
 */
void dsm_ref_link(u64 *ref_loc, u64 target_off)
{
    struct dsm_ref_obj *obj = (struct dsm_ref_obj *)off_to_ptr(target_off);
    mid_t mid = CUR_MACHINE_ID;
    u64 old_ri, new_ri;

    do {
        u16 saw_lcid, old_cnt;
        u32 saw_era, cur_era;

        old_ri   = (u64)atomic_load_64((s64 *)&obj->ref_info);
        old_cnt  = dsm_ref_cnt(old_ri);
        saw_lcid = dsm_ref_lcid(old_ri);
        saw_era  = dsm_ref_era(old_ri);
        cur_era  = era_self();

        /*
         * Era table maintenance (§4.2 of CXL-SHM):
         * Record the era embedded in the object so other machines can
         * later determine whether *our* in-flight op committed.
         */
        if (saw_era > era_get(mid, (mid_t)saw_lcid))
            era_set(mid, (mid_t)saw_lcid, saw_era);

        /*
         * Write redo log entry before the CAS.
         * If we crash after this write but before the CAS, recovery will
         * find this entry and check whether the CAS actually landed.
         */
        write_redo(DSM_REF_LINK_OP, old_cnt, cur_era,
                   ptr_to_off(ref_loc), target_off);

        new_ri = dsm_ref_pack(mid, old_cnt + 1, cur_era);

    } while (!atomic_bool_compare_exchange_64(
                 (s64 *)&obj->ref_info, (s64)old_ri, (s64)new_ri));

    /*
     * CAS succeeded: write the pointer.  A crash here (after CAS, before
     * this store) is recoverable — recovery will re-apply this write.
     */
    *ref_loc = target_off;
    smp_mb();

    era_self_inc();
}

/*
 * dsm_ref_unlink — decrement ref_cnt, zero pointer, optionally recurse.
 *
 * Returns 1 if ref_cnt hit 0 (caller should free the object), else 0.
 */
int dsm_ref_unlink(u64 *ref_loc, u64 target_off)
{
    struct dsm_ref_obj *obj = (struct dsm_ref_obj *)off_to_ptr(target_off);
    mid_t mid = CUR_MACHINE_ID;
    u64 old_ri, new_ri;
    u16 old_cnt;

    do {
        u16 saw_lcid;
        u32 saw_era, cur_era;

        old_ri   = (u64)atomic_load_64((s64 *)&obj->ref_info);
        old_cnt  = dsm_ref_cnt(old_ri);
        saw_lcid = dsm_ref_lcid(old_ri);
        saw_era  = dsm_ref_era(old_ri);
        cur_era  = era_self();

        if (saw_era > era_get(mid, (mid_t)saw_lcid))
            era_set(mid, (mid_t)saw_lcid, saw_era);

        write_redo(DSM_REF_UNLINK_OP, old_cnt, cur_era,
                   ptr_to_off(ref_loc), target_off);

        new_ri = dsm_ref_pack(mid, old_cnt - 1, cur_era);

    } while (!atomic_bool_compare_exchange_64(
                 (s64 *)&obj->ref_info, (s64)old_ri, (s64)new_ri));

    *ref_loc = 0;
    smp_mb();

    era_self_inc();

    if (old_cnt - 1 == 0) {
        /* Recursively unlink embedded refs before the caller frees. */
        u64 i;
        u64 *embedded = (u64 *)(obj + 1);
        for (i = 0; i < obj->embedded_ref_cnt; i++) {
            if (embedded[i])
                dsm_ref_unlink(&embedded[i], embedded[i]);
        }
        return 1;
    }
    return 0;
}

/*
 * dsm_ref_recover_machine — recovery entry point for a confirmed-dead machine.
 *
 * Algorithm (§4.3 of CXL-SHM, machine-granular):
 *
 *   1. Find the redo entry with the highest cur_era for `dead_mid`.
 *      That entry represents the last in-flight operation.
 *
 *   2. Determine whether the CAS committed ("need_redo" check):
 *
 *      Case A – obj->ref_info still shows lcid==dead_mid, era==redo->cur_era:
 *        The CAS landed.  The pointer write (*ref_loc = target_off / 0) may
 *        have been lost.  Re-apply it.
 *
 *      Case B – some live machine j has era[j][dead_mid] >= redo->cur_era:
 *        Machine j observed the ref_info written by dead_mid (it updated
 *        its era table when it saw dead_mid's era in an object).  So the
 *        CAS landed.  Re-apply the pointer write.
 *
 *      Case C – neither A nor B:
 *        The CAS never committed.  The ref_info was not changed.  Nothing
 *        to do.
 *
 *   3. For UNLINK where saved_cnt==1 (ref_cnt → 0): recursively unlink
 *      any embedded refs that the dead machine was about to clean up.
 *
 *   4. Clear the dead machine's slot (mid_token = 0).
 */
void dsm_ref_recover_machine(mid_t dead_mid)
{
    struct dsm_ref_redo *redo;
    struct dsm_ref_obj  *obj;
    int need_redo = 0;

    dsm_info("[DSM_REF] recovering machine %d\n", dead_mid);

    redo = find_latest_redo(dead_mid);
    if (!redo || redo->func_id == 0) {
        dsm_info("[DSM_REF] machine %d: no redo entry\n", dead_mid);
        goto clear;
    }

    obj = (struct dsm_ref_obj *)off_to_ptr(redo->refed_off);

    /* ---- era-based check ---- */
    {
        u64  ri      = (u64)atomic_load_64((s64 *)&obj->ref_info);
        u16  lcid    = dsm_ref_lcid(ri);
        u32  obj_era = dsm_ref_era(ri);
        mid_t j;

        /* Case A */
        if ((mid_t)lcid == dead_mid && obj_era == redo->cur_era) {
            need_redo = 1;
            goto apply;
        }

        /* Case B */
        for (j = 0; j < (mid_t)CLUSTER_MACHINE_NUM; j++) {
            if (j == dead_mid)
                continue;
            if (era_get(j, dead_mid) >= redo->cur_era) {
                need_redo = 1;
                goto apply;
            }
        }
        /* Case C: fall through */
    }

apply:
    if (!need_redo) {
        dsm_info("[DSM_REF] machine %d: CAS never committed (era=%u), skip\n",
                 dead_mid, redo->cur_era);
        goto clear;
    }

    if (redo->func_id == DSM_REF_LINK_OP) {
        /*
         * CAS incremented cnt.  Re-apply the pointer write that may
         * have been lost.
         */
        u64 *ref_loc = (u64 *)off_to_ptr(redo->ref_loc_off);
        *ref_loc = redo->refed_off;
        smp_mb();
        dsm_info("[DSM_REF] machine %d: redid LINK "
                 "ref_loc_off=0x%llx → 0x%llx\n",
                 dead_mid,
                 (unsigned long long)redo->ref_loc_off,
                 (unsigned long long)redo->refed_off);

    } else { /* DSM_REF_UNLINK_OP */
        /*
         * CAS decremented cnt.  Re-apply the zero write to the pointer.
         */
        u64 *ref_loc = (u64 *)off_to_ptr(redo->ref_loc_off);
        *ref_loc = 0;
        smp_mb();
        dsm_info("[DSM_REF] machine %d: redid UNLINK "
                 "ref_loc_off=0x%llx\n",
                 dead_mid,
                 (unsigned long long)redo->ref_loc_off);

        /*
         * If this unlink was the last reference (saved_cnt == 1), the
         * dead machine was about to clean up embedded refs.  Do it now
         * on its behalf.
         */
        if (redo->saved_cnt == 1) {
            u64  i;
            u64 *embedded = (u64 *)(obj + 1);
            for (i = 0; i < obj->embedded_ref_cnt; i++) {
                if (embedded[i]) {
                    dsm_ref_unlink(&embedded[i], embedded[i]);
                }
            }
        }
    }

clear:
    /* Release the dead machine's slot. */
    atomic_store_64((s64 *)&rmeta()->machine[dead_mid].mid_token, 0);
    smp_mb();
    dsm_info("[DSM_REF] machine %d recovery done\n", dead_mid);
}

#endif /* DSM_ENABLED */
