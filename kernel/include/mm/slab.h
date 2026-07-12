#pragma once

#include <common/list.h>

/*
 * order range: [SLAB_MIN_ORDER, SLAB_MAX_ORDER]
 * ChCore prepares the slab for each order in the range.
 */
#define SLAB_MIN_ORDER (5)
#define SLAB_MAX_ORDER (11)

/* The size of one slab is 2M. */
#define SIZE_OF_ONE_SLAB (2 * 1024 * 1024)

#ifdef SLAB_CRASH_RECOVERY
/* Undo log op types */
#define SLAB_OP_NONE  0
#define SLAB_OP_ALLOC 1
#define SLAB_OP_FREE  2

/*
 * Per-CPU in-flight undo log for crash recovery.
 * Lives in CXL (dsm_meta) so it survives crashes.
 * Each CPU can only have one slab op in-flight (holds lock).
 */
struct slab_cpu_log {
    volatile u8   op;           /* SLAB_OP_NONE / ALLOC / FREE */
    u8   _pad[5];
    u16  old_free_cnt;          /* current_free_cnt before mutation */
    void *slab_addr;            /* which slab is being operated on */
    void *old_free_head;        /* free_list_head before mutation */
};
#endif /* SLAB_CRASH_RECOVERY */

/* slab_header resides at the beginning of each slab and may occupy multiple
 * smallest-object slots depending on sizeof(struct slab_header). */
struct slab_header {
    /* The list of free slots, which can be converted to struct
     * slab_slot_list. */
    void *free_list_head;
    /* Partial/full slab list node. */
    struct list_head node;

    int order;
    unsigned short owner_cpu;
    unsigned short total_free_cnt; /* MAX: 65536 */
    unsigned short current_free_cnt;
};

/* Each free slot in one slab is regarded as slab_slot_list. */
struct slab_slot_list {
    void *next_free;
};

struct slab_pointer {
    struct slab_header *current_slab;
    struct list_head partial_slab_list;
#ifdef SLAB_CRASH_RECOVERY
    struct list_head full_slab_list;
#endif
};

/* All interfaces are kernel/mm module internal interfaces. */
void init_slab(void);
void *alloc_in_slab(unsigned long);
void free_in_slab(void *addr);
unsigned long get_free_mem_size_from_slab(void);

/* DRAM slabs */
void init_dram_slab(void);
void *alloc_in_dram_slab(unsigned long);
void free_in_dram_slab(void *addr);

/* CXL slabs */
void init_cxl_slab(void);
void *alloc_in_cxl_slab(unsigned long);
void free_in_cxl_slab(void *addr);

#ifdef DSM_LINEAR_MM_LAYOUT
/* TEMP slabs */
void init_temp_slab(void);
void *alloc_in_temp_slab(unsigned long);
void free_in_temp_slab(void *addr);
#endif

/* common */
static inline int size_to_order(unsigned long size)
{
    unsigned long order = 0;
    unsigned long tmp = size;

    while (tmp > 1) {
        tmp >>= 1;
        order += 1;
    }
    if (size > (1 << order))
        order += 1;

    return (int)order;
}

static inline unsigned long order_to_size(int order)
{
    return 1UL << order;
}

/* @set_or_clear: true for set and false for clear. */
void set_or_clear_slab_in_page(void *addr, unsigned long size,
                               bool set_or_clear);

#ifdef SLAB_CRASH_RECOVERY
#include <common/mem_sync.h>

/*
 * Per-CPU log helpers. The log pointer is obtained from dsm_meta
 * by the caller and passed in.
 */
/*
 * Optimized persistence protocol (2 FENCE instead of 4):
 *
 * 1. log_begin: write log data → FLUSH → FENCE (log persistent),
 *    then set op → FLUSH (no FENCE: persist_header's FENCE covers it).
 *    Crash before persist_header: op may/may not be persistent;
 *    if persistent, recovery undoes an unmodified slab (harmless).
 *
 * 2. persist_header: FLUSH headers → FENCE (ensures both slab header
 *    AND op are persistent before we clear op).
 *
 * 3. log_end: clear op → FLUSH (no FENCE: next operation's log_begin
 *    FENCE will order this; per-CPU lock prevents concurrent access).
 */
static inline void slab_log_begin(struct slab_cpu_log *log,
                                  struct slab_header *slab, u8 op)
{
    log->slab_addr      = (void *)slab;
    log->old_free_head  = slab->free_list_head;
    log->old_free_cnt   = slab->current_free_cnt;
    FLUSH(log);
    FENCE;          /* log data must be persistent before op is set */
    log->op = op;
    FLUSH(&log->op);
    /* No FENCE: persist_header's FENCE ensures op is persistent */
}

static inline void slab_log_end(struct slab_cpu_log *log)
{
    log->op = SLAB_OP_NONE;
    FLUSH(&log->op);
    /* No FENCE: next log_begin's FENCE orders this;
     * per-CPU lock prevents concurrent access */
}

static inline void slab_persist_header(struct slab_header *slab)
{
    FLUSH(&slab->free_list_head);
    FLUSH(&slab->current_free_cnt);
    FENCE;  /* ensures slab header + op are persistent before log clear */
}

void recover_cxl_slabs(void);
#else
#define slab_log_begin(log, slab, op)   do {} while(0)
#define slab_log_end(log)               do {} while(0)
#define slab_persist_header(slab)       do {} while(0)
#endif /* SLAB_CRASH_RECOVERY */

#if TRACK_THREAD_MM == ON
u64 size_to_slab_order(u64 size);
#endif
