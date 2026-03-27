
#pragma once
#ifndef DSM_REF_H
#define DSM_REF_H

/*
 * dsm_ref: Per-machine redo-log + era-based partial-failure-resilient
 *          reference counting for CXL shared memory objects.
 *
 * Failure model: whole-machine failure only (all CPUs on a machine fail
 * together).  A surviving machine can recover ref-count state left by a
 * dead machine without blocking and without a global lock.
 *
 * Mechanism (simplified from CXL-SHM, Zhang et al. 2023):
 *
 *  ref_info  [lcid:16 | cnt:16 | era:32]  (per-object, in SHM)
 *    lcid – machine_id of the last modifier
 *    cnt  – reference count
 *    era  – lcid's own era at the time of the last modification
 *
 *  era[i][j]  "last era of machine j observed by machine i"  (in SHM)
 *    Updated before every CAS so survivors can tell whether a redo
 *    log entry was committed before the crash.
 *
 *  redo log   one ring-buffer slot per machine (DSM_REF_REDO_CNT entries)
 *    Written into CXL SHM BEFORE each CAS on ref_info.
 *    Recovery reads the highest-era entry and re-applies if needed.
 */

#include <common/types.h>

/* ---- tunables ---------------------------------------------------------- */

#define DSM_REF_REDO_CNT     8   /* ring-buffer depth; must be power of 2 */

/*
 * DSM_REF_MAX_MACHINES: upper bound on cluster size used to size the era
 * matrix and machine-state array.  Must be >= CLUSTER_MAX_MACHINE_NUM.
 * Defined here (not derived from dsm-single.h) to avoid a circular include.
 */
#ifndef DSM_REF_MAX_MACHINES
#define DSM_REF_MAX_MACHINES 8
#endif

/* ---- operation IDs for the redo log ------------------------------------ */

#define DSM_REF_LINK_OP   1   /* increment ref_cnt, write pointer */
#define DSM_REF_UNLINK_OP 2   /* decrement ref_cnt, zero pointer  */

/* ---- redo log entry: exactly one cache line (64 bytes) ----------------- */

struct dsm_ref_redo {
    u16 func_id;       /* DSM_REF_{LINK,UNLINK}_OP; 0 = empty slot  */
    u16 saved_cnt;     /* ref_cnt value *before* this operation      */
    u32 cur_era;       /* machine's own era when the op was written  */
    u64 ref_loc_off;   /* SHM offset of the u64 pointer being updated*/
    u64 refed_off;     /* SHM offset of the target dsm_ref_obj       */
    u8  _pad[40];      /* pad to 64 bytes                            */
} __attribute__((aligned(64)));

/* ---- per-machine state stored in CXL SHM ------------------------------ */

struct dsm_ref_machine_state {
    volatile u64 mid_token;                    /* 0=free; mid+1=joined  */
    u8  _pad[56];                              /* rest of first cacheline*/
    struct dsm_ref_redo redo[DSM_REF_REDO_CNT];/* 8 × 64 = 512 bytes    */
} __attribute__((aligned(64)));

/* ---- global ref metadata (embedded in dsm_metadata_t) ----------------- */

typedef struct {
    /*
     * era[i][j]: last era of machine j seen by machine i.
     * DSM_REF_MAX_MACHINES × DSM_REF_MAX_MACHINES × 4 = 256 bytes (for 8).
     */
    volatile u32 era[DSM_REF_MAX_MACHINES][DSM_REF_MAX_MACHINES];

    /* Per-machine redo log + liveness token. */
    struct dsm_ref_machine_state machine[DSM_REF_MAX_MACHINES];
} dsm_ref_meta_t;

/* ---- object header prepended to every ref-counted SHM object ---------- */

struct dsm_ref_obj {
    volatile u64 ref_info;
    u64          embedded_ref_cnt; /* # of u64 offset fields after header */
    /* user data starts here */
};

/* ---- ref_info packing helpers ----------------------------------------- */

static inline u64 dsm_ref_pack(u16 lcid, u16 cnt, u32 era)
{
    return ((u64)lcid << 48) | ((u64)cnt << 32) | (u64)era;
}
static inline u16 dsm_ref_lcid(u64 ri) { return (u16)(ri >> 48); }
static inline u16 dsm_ref_cnt (u64 ri) { return (u16)(ri >> 32); }
static inline u32 dsm_ref_era (u64 ri) { return (u32)ri; }

/* ---- public API -------------------------------------------------------- */

/*
 * dsm_ref_global_init()
 *   Called once by machine 0 after dsm_meta is mapped.
 *   Zeroes the era matrix and all machine slots.
 */
void dsm_ref_global_init(void);

/*
 * dsm_ref_machine_join()
 *   Called by each machine at boot.  Registers CUR_MACHINE_ID, clears any
 *   stale redo state, and kicks the machine's own era to 1.
 */
void dsm_ref_machine_join(void);

/*
 * dsm_ref_machine_leave()
 *   Called on graceful shutdown.  Zeroes the redo log and releases the slot.
 */
void dsm_ref_machine_leave(void);

/*
 * dsm_ref_link(ref_loc, target_off)
 *   Atomically increment ref_cnt of the object at SHM offset `target_off`,
 *   then store `target_off` into `*ref_loc` (which must be a u64 field
 *   inside CXL SHM).
 *
 *   Writes a redo log entry before the CAS so a crash is recoverable.
 */
void dsm_ref_link(u64 *ref_loc, u64 target_off);

/*
 * dsm_ref_unlink(ref_loc, target_off)
 *   Atomically decrement ref_cnt and zero `*ref_loc`.
 *   Recursively unlinks embedded refs when cnt reaches 0.
 *
 *   Returns 1 if ref_cnt hit 0 (caller should free the memory), 0 otherwise.
 */
int dsm_ref_unlink(u64 *ref_loc, u64 target_off);

/*
 * dsm_ref_recover_machine(dead_mid)
 *   Called by the monitor when machine `dead_mid` is confirmed dead
 *   (e.g. heartbeat timeout).  Applies the era-based check to determine
 *   whether the last redo log entry committed, and re-applies the pointer
 *   write if needed.  Clears the dead machine's slot when done.
 */
void dsm_ref_recover_machine(mid_t dead_mid);

#endif /* DSM_REF_H */
