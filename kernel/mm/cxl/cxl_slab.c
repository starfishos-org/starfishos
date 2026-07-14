#include <common/macro.h>
#include <common/types.h>
#include <common/kprint.h>
#include <common/lock.h>
#include <arch/sync.h>
#include <mm/kmalloc.h>
#include <mm/slab.h>
#include <mm/buddy.h>
#include <mm/nvm.h>
#ifdef SLAB_CRASH_RECOVERY
#include <common/mem_sync.h>
#endif
#ifdef DSM_ENABLED
#include <dsm/dsm-single.h>
#endif

/*
 * When SLAB_CRASH_RECOVERY is ON, pools/locks/logs live in dsm_meta (CXL).
 * When OFF, they are plain DRAM globals (original behavior).
 */
#ifdef SLAB_CRASH_RECOVERY

#define CXL_SLAB_META  (dsm_meta->cxl_slab_meta[CUR_MACHINE_ID])
#define cxl_slab_pool   (CXL_SLAB_META.pool)
#define cxl_cpu_logs    (CXL_SLAB_META.cpu_logs)

/*
 * Keep per-CPU locks in DRAM even with crash recovery — they are
 * re-initialized in Phase 2 of recovery, so persistence is unnecessary.
 * This avoids expensive CXL atomics on every lock acquire/release.
 * Padded stride prevents false sharing between CPUs.
 */
#define SLAB_PADDED_STRIDE 16
static struct lock cxl_slabs_locks[PLAT_CPU_NUM][SLAB_PADDED_STRIDE];

#else /* !SLAB_CRASH_RECOVERY */

/*
 * Per-CPU CXL slab pools/locks in DRAM.
 * Stride padded to 16 entries so each CPU's slice is 64-byte aligned,
 * preventing false sharing. (Only indices [SLAB_MIN_ORDER..SLAB_MAX_ORDER]
 * are used; the padding slots are never accessed.)
 */
#define SLAB_PADDED_STRIDE 16
struct slab_pointer cxl_slab_pool[PLAT_CPU_NUM][SLAB_PADDED_STRIDE];
static struct lock cxl_slabs_locks[PLAT_CPU_NUM][SLAB_PADDED_STRIDE];

#endif /* SLAB_CRASH_RECOVERY */

#ifdef DSM_ENABLED
static void cxl_slab_drain_remote_free(void);
#endif

#if CHECK_FREE_COUNT_IN_SLAB == ON
static bool check_slot_free_count(struct slab_header *slab)
{
    struct slab_slot_list *cur_slot;
    unsigned long cnt = 0;

    if (slab == NULL)
        return 0;

    cur_slot = (struct slab_slot_list *)(slab->free_list_head);
    while (cur_slot != NULL) {
        cnt++;
        cur_slot = (struct slab_slot_list *)cur_slot->next_free;
    }

    if (cnt != slab->current_free_cnt) {
        kwarn("slab system: free count is not correct. cnt: %d, free_cnt: %d\n",
              cnt,
              slab->current_free_cnt);
        return 1;
    }

    return 0;
}
#endif

static void *alloc_slab_memory(unsigned long size)
{
    void *addr;
    int order;

    /* Allocate memory pages from the buddy system as a new slab. */
    order = size_to_order(size / BUDDY_PAGE_SIZE);
    addr = get_cxl_pages(order);

    if (unlikely(addr == NULL)) {
        kwarn("%s failed due to out of memory\n", __func__);
        return NULL;
    }

    set_or_clear_slab_in_page(addr, size, true);

    return addr;
}

static struct slab_header *init_slab_cache(int order, int size, u32 owner_cpu)
{
    void *addr;
    struct slab_slot_list *slot;
    struct slab_header *slab;
    unsigned long cnt, obj_size, total_slots, meta_slots;
    unsigned long i;

    addr = alloc_slab_memory(size);
    if (unlikely(addr == NULL))
        /* Fail: no available memory. */
        return NULL;
    slab = (struct slab_header *)addr;

    obj_size = order_to_size(order);
    total_slots = size / obj_size;
    /*
     * slab_header may be larger than one smallest object slot (e.g. 40B
     * header vs 32B slot). Reserve enough whole slots for metadata.
     */
    meta_slots = ROUND_UP(sizeof(struct slab_header), obj_size) / obj_size;
    BUG_ON(meta_slots == 0 || meta_slots >= total_slots);
    cnt = total_slots - meta_slots;

    slot = (struct slab_slot_list *)((unsigned long)addr + meta_slots * obj_size);
    slab->free_list_head = (void *)slot;
    slab->order = order;
    slab->owner_cpu = owner_cpu;
#ifdef DSM_ENABLED
    slab->owner_machine = (unsigned short)CUR_MACHINE_ID;
#else
    slab->owner_machine = 0;
#endif
    slab->total_free_cnt = cnt;
    slab->current_free_cnt = cnt;

    /* The last slot has no next one. */
    for (i = 0; i < cnt - 1; ++i) {
        slot->next_free = (void *)((unsigned long)slot + obj_size);
        slot = (struct slab_slot_list *)((unsigned long)slot + obj_size);
    }
    slot->next_free = NULL;

    return slab;
}

static void choose_new_current_slab(struct slab_pointer *pool, int order)
{
    struct list_head *list;

    list = &(pool->partial_slab_list);
    if (list_empty(list)) {
        pool->current_slab = NULL;
    } else {
        struct slab_header *slab;

        slab = (struct slab_header *)list_entry(
                list->next, struct slab_header, node);
        pool->current_slab = slab;
        list_del(list->next);
    }
}

static void *alloc_in_cxl_slab_impl(int order)
{
    struct slab_header *current_slab;
    struct slab_slot_list *free_list;
    void *next_slot;
    u32 cpu_id = smp_get_cpu_id();

    BUG_ON(cpu_id >= PLAT_CPU_NUM);
    lock(&cxl_slabs_locks[cpu_id][order]);

    current_slab = cxl_slab_pool[cpu_id][order].current_slab;
#if CHECK_FREE_COUNT_IN_SLAB == ON
    check_slot_free_count(current_slab);
#endif
    /* When serving the first allocation request. */
    if (unlikely(current_slab == NULL)) {
        current_slab = init_slab_cache(order, SIZE_OF_ONE_SLAB, cpu_id);
        if (current_slab == NULL) {
            unlock(&cxl_slabs_locks[cpu_id][order]);
            return NULL;
        }
        cxl_slab_pool[cpu_id][order].current_slab = current_slab;
    }

    free_list = (struct slab_slot_list *)current_slab->free_list_head;
    BUG_ON(free_list == NULL);

    slab_log_begin(&cxl_cpu_logs[cpu_id], current_slab, SLAB_OP_ALLOC);

    next_slot = free_list->next_free;
    current_slab->free_list_head = next_slot;

    current_slab->current_free_cnt -= 1;
    /* When current_slab is full, choose a new slab as the current one. */
    if (unlikely(current_slab->current_free_cnt == 0)) {
        choose_new_current_slab(&cxl_slab_pool[cpu_id][order], order);
#ifdef SLAB_CRASH_RECOVERY
        /* Track full slab so recovery can find it */
        list_append(&current_slab->node,
                    &cxl_slab_pool[cpu_id][order].full_slab_list);
#endif
    }

    slab_persist_header(current_slab);
    slab_log_end(&cxl_cpu_logs[cpu_id]);

    unlock(&cxl_slabs_locks[cpu_id][order]);

    return (void *)free_list;
}

#if DETECTING_DOUBLE_FREE_IN_SLAB == ON
static int check_slot_is_free(struct slab_header *slab_header,
                              struct slab_slot_list *slot)
{
    struct slab_slot_list *cur_slot;
    struct slab_header *next_slab;

    UNUSED(next_slab);

    cur_slot = (struct slab_slot_list *)(slab_header->free_list_head);

    while (cur_slot != NULL) {
        if (cur_slot == slot)
            return 1;
        cur_slot = (struct slab_slot_list *)cur_slot->next_free;
    }

    return 0;
}
#endif

static void try_insert_full_slab_to_partial(struct slab_header *slab)
{
    /* @slab is not a full one. */
    if (slab->current_free_cnt != 0)
        return;

    int order;
    u32 owner_cpu;
    order = slab->order;
    owner_cpu = slab->owner_cpu;
    BUG_ON(owner_cpu >= PLAT_CPU_NUM);

#ifdef SLAB_CRASH_RECOVERY
    /* Remove from full list first */
    list_del(&slab->node);
#endif
    list_append(&slab->node, &cxl_slab_pool[owner_cpu][order].partial_slab_list);
}

static void try_return_slab_to_buddy(struct slab_header *slab, int order)
{
    u32 owner_cpu = slab->owner_cpu;
    BUG_ON(owner_cpu >= PLAT_CPU_NUM);

    /* The slab is whole free now. */
    if (slab->current_free_cnt != slab->total_free_cnt)
        return;

    if (slab == cxl_slab_pool[owner_cpu][order].current_slab)
        choose_new_current_slab(&cxl_slab_pool[owner_cpu][order], order);
    else
        list_del(&slab->node);

    /* Clear the slab field in the page structures before freeing them. */
    set_or_clear_slab_in_page(slab, SIZE_OF_ONE_SLAB, false);
    free_pages(slab);
}

/* Interfaces exported to the kernel/mm moudule */

void init_cxl_slab(void)
{
    int order;
    u32 cpu_id;

    /* slab obj size: 32, 64, 128, 256, 512, 1024, 2048 */
    for (cpu_id = 0; cpu_id < PLAT_CPU_NUM; cpu_id++) {
        for (order = SLAB_MIN_ORDER; order <= SLAB_MAX_ORDER; order++) {
            lock_init(&cxl_slabs_locks[cpu_id][order]);
            cxl_slab_pool[cpu_id][order].current_slab = NULL;
            init_list_head(&(cxl_slab_pool[cpu_id][order].partial_slab_list));
#ifdef SLAB_CRASH_RECOVERY
            init_list_head(&(cxl_slab_pool[cpu_id][order].full_slab_list));
#endif
        }
#ifdef SLAB_CRASH_RECOVERY
        cxl_cpu_logs[cpu_id].op = SLAB_OP_NONE;
#endif
    }
#ifdef DSM_ENABLED
    /*
     * Clear only our own remote-free stack: objects owned by this machine
     * cannot exist before it boots, and other machines' stacks may hold
     * live entries.
     */
    dsm_meta->cxl_slab_remote_free[CUR_MACHINE_ID].head = NULL;
#endif
    kdebug("mm: finish initing slab allocators\n");
}

void *alloc_in_cxl_slab(unsigned long size)
{
    int order;

    BUG_ON(size > order_to_size(SLAB_MAX_ORDER));

#ifdef DSM_ENABLED
    /* Reclaim objects other machines freed into our slabs. */
    cxl_slab_drain_remote_free();
#endif

    order = (int)size_to_order(size);
    if (order < SLAB_MIN_ORDER)
        order = SLAB_MIN_ORDER;

    return alloc_in_cxl_slab_impl(order);
}

static void free_in_cxl_slab_local(void *addr)
{
    struct page *page;
    struct slab_header *slab;
    struct slab_slot_list *slot;
    int order;
    u32 owner_cpu;

    slot = (struct slab_slot_list *)addr;
    page = virt_to_page(addr);
    BUG_ON(page == NULL);

    slab = page->slab;
    order = slab->order;
    owner_cpu = slab->owner_cpu;
    BUG_ON(owner_cpu >= PLAT_CPU_NUM);
    lock(&cxl_slabs_locks[owner_cpu][order]);

    try_insert_full_slab_to_partial(slab);

#if DETECTING_DOUBLE_FREE_IN_SLAB == ON
    /*
     * SLAB double free detection: check whether the slot to free is
     * already in the free list.
     */
    if (check_slot_is_free(slab, slot) == 1) {
        kinfo("SLAB: double free detected. Address is %p\n",
              (unsigned long)slot);
        BUG_ON(1);
    }
#endif

    slab_log_begin(&cxl_cpu_logs[owner_cpu], slab, SLAB_OP_FREE);

    slot->next_free = slab->free_list_head;
    slab->free_list_head = slot;
    slab->current_free_cnt += 1;

    slab_persist_header(slab);
    slab_log_end(&cxl_cpu_logs[owner_cpu]);

    try_return_slab_to_buddy(slab, order);

#if CHECK_FREE_COUNT_IN_SLAB == ON
    check_slot_free_count(slab);
#endif

    unlock(&cxl_slabs_locks[owner_cpu][order]);
}

#ifdef DSM_ENABLED
/*
 * Cross-machine frees: the per-CPU pools/locks above are machine-local,
 * so a machine must never touch another machine's slab lists. Instead it
 * pushes the object onto the owner machine's lock-free Treiber stack in
 * dsm_meta (the link is stored in the freed slot itself); the owner
 * drains the stack on its own alloc/free path, keeping every slab-list
 * mutation under the owner's local locks.
 */
static void cxl_slab_remote_push(u32 owner_machine, void *addr)
{
    struct slab_slot_list *slot = (struct slab_slot_list *)addr;
    void *volatile *headp = &dsm_meta->cxl_slab_remote_free[owner_machine].head;
    void *old;

    do {
        old = *headp;
        slot->next_free = old;
    } while (compare_and_swap_64((s64 *)headp, (s64)old, (s64)slot)
             != (s64)old);
}

static void cxl_slab_drain_remote_free(void)
{
    struct slab_slot_list *slot, *next;

    if (dsm_meta->cxl_slab_remote_free[CUR_MACHINE_ID].head == NULL)
        return;

    slot = (struct slab_slot_list *)atomic_exchange_64(
            (void *)&dsm_meta->cxl_slab_remote_free[CUR_MACHINE_ID].head, 0);
    while (slot != NULL) {
        next = (struct slab_slot_list *)slot->next_free;
        free_in_cxl_slab_local(slot);
        slot = next;
    }
}
#endif /* DSM_ENABLED */

void free_in_cxl_slab(void *addr)
{
#ifdef DSM_ENABLED
    struct page *page;
    struct slab_header *slab;

    page = virt_to_page(addr);
    BUG_ON(page == NULL);
    slab = page->slab;

    if ((int)slab->owner_machine != CUR_MACHINE_ID) {
        BUG_ON(slab->owner_machine >= CLUSTER_MAX_MACHINE_NUM);
        cxl_slab_remote_push(slab->owner_machine, addr);
        return;
    }

    cxl_slab_drain_remote_free();
#endif
    free_in_cxl_slab_local(addr);
}

/* This interface is not marked as static because it is needed in the unit test.
 */
unsigned long cxl_get_free_slot_number(int order)
{
    struct slab_header *slab;
    struct slab_slot_list *slot;
    unsigned long current_slot_num = 0;
    unsigned long check_slot_num = 0;
    u32 cpu_id;

    for (cpu_id = 0; cpu_id < PLAT_CPU_NUM; cpu_id++) {
        lock(&cxl_slabs_locks[cpu_id][order]);

        slab = cxl_slab_pool[cpu_id][order].current_slab;
        if (slab) {
            slot = (struct slab_slot_list *)slab->free_list_head;
            while (slot != NULL) {
                current_slot_num++;
                slot = slot->next_free;
            }
            check_slot_num += slab->current_free_cnt;
        }

        if (!list_empty(&cxl_slab_pool[cpu_id][order].partial_slab_list)) {
            for_each_in_list (slab,
                              struct slab_header,
                              node,
                              &cxl_slab_pool[cpu_id][order].partial_slab_list) {
                slot = (struct slab_slot_list *)slab->free_list_head;
                while (slot != NULL) {
                    current_slot_num++;
                    slot = slot->next_free;
                }
                check_slot_num += slab->current_free_cnt;
            }
        }

        unlock(&cxl_slabs_locks[cpu_id][order]);
    }

    BUG_ON(check_slot_num != current_slot_num);

    return current_slot_num;
}

#ifdef SLAB_CRASH_RECOVERY
/*
 * Collect all slabs from a pool (current + partial + full) into a flat list,
 * then clear the pool.
 */
static void collect_all_slabs(struct slab_pointer *pool,
                              struct list_head *collect)
{
    struct slab_header *slab, *tmp;

    /* Move current_slab */
    if (pool->current_slab) {
        list_append(&pool->current_slab->node, collect);
        pool->current_slab = NULL;
    }

    /* Move partial list */
    if (!list_empty(&pool->partial_slab_list)) {
        __for_each_in_list_safe (slab, tmp, struct slab_header, node,
                               &pool->partial_slab_list) {
            list_del(&slab->node);
            list_append(&slab->node, collect);
        }
    }

    /* Move full list */
    if (!list_empty(&pool->full_slab_list)) {
        __for_each_in_list_safe (slab, tmp, struct slab_header, node,
                               &pool->full_slab_list) {
            list_del(&slab->node);
            list_append(&slab->node, collect);
        }
    }
}

/*
 * Recovery: undo in-flight slab operations, clear locks, rebuild pool lists.
 * Called during init when recovering from a crash.
 * Walks per-CPU logs + pool lists in CXL — no full page scan needed.
 */
void recover_cxl_slabs(void)
{
    u32 cpu_id;
    int order, mid;
    struct slab_header *slab, *tmp;
    struct slab_cpu_log *log;
    struct list_head all_slabs;

    kinfo("[SLAB RECOVERY] Starting CXL slab recovery...\n");

    /* Recover all machines' slab state (each machine's pools are in CXL) */
    for (mid = 0; mid < (int)dsm_meta->cluster_machine_num; mid++) {

        /* Phase 1: Undo in-flight operations via per-CPU logs */
        for (cpu_id = 0; cpu_id < PLAT_CPU_NUM; cpu_id++) {
            log = &dsm_meta->cxl_slab_meta[mid].cpu_logs[cpu_id];
            if (log->op != SLAB_OP_NONE) {
                slab = (struct slab_header *)log->slab_addr;
                kinfo("[SLAB RECOVERY] M%d CPU%d: undoing op=%d on slab %p\n",
                      mid, cpu_id, log->op, slab);
                slab->free_list_head   = log->old_free_head;
                slab->current_free_cnt = log->old_free_cnt;
                FLUSH(slab);
                FENCE;
                log->op = SLAB_OP_NONE;
                FLUSH(&log->op);
                FENCE;
            }
        }

        /* Phase 2: Clear all locks */
        for (cpu_id = 0; cpu_id < PLAT_CPU_NUM; cpu_id++)
            for (order = SLAB_MIN_ORDER; order <= SLAB_MAX_ORDER; order++)
                lock_init(&dsm_meta->cxl_slab_meta[mid].locks[cpu_id][order]);

        /* Phase 3: Rebuild pool lists from collected slabs */
        for (cpu_id = 0; cpu_id < PLAT_CPU_NUM; cpu_id++) {
            for (order = SLAB_MIN_ORDER; order <= SLAB_MAX_ORDER; order++) {
                struct slab_pointer *pool =
                    &dsm_meta->cxl_slab_meta[mid].pool[cpu_id][order];

                /* Collect all slabs from this pool */
                init_list_head(&all_slabs);
                collect_all_slabs(pool, &all_slabs);

                /* Re-init pool */
                pool->current_slab = NULL;
                init_list_head(&pool->partial_slab_list);
                init_list_head(&pool->full_slab_list);

                /* Re-classify each slab */
                __for_each_in_list_safe (slab, tmp, struct slab_header, node,
                                       &all_slabs) {
                    list_del(&slab->node);

                    if (slab->current_free_cnt == slab->total_free_cnt) {
                        /* Fully free: return to buddy */
                        set_or_clear_slab_in_page(slab, SIZE_OF_ONE_SLAB,
                                                  false);
                        free_pages(slab);
                    } else if (slab->current_free_cnt == 0) {
                        /* Full slab */
                        list_append(&slab->node, &pool->full_slab_list);
                    } else {
                        /* Partial slab */
                        if (pool->current_slab == NULL)
                            pool->current_slab = slab;
                        else
                            list_append(&slab->node,
                                        &pool->partial_slab_list);
                    }
                }
            }
        }
    }

    kinfo("[SLAB RECOVERY] CXL slab recovery complete.\n");
}
#endif /* SLAB_CRASH_RECOVERY */

/* Get the size of free memory in slab */
unsigned long cxl_get_free_mem_size_from_slab(void)
{
    int order;
    unsigned long current_slot_size;
    unsigned long slot_num;
    unsigned long total_size = 0;

    for (order = SLAB_MIN_ORDER; order <= SLAB_MAX_ORDER; order++) {
        current_slot_size = order_to_size(order);
        slot_num = cxl_get_free_slot_number(order);
        total_size += (current_slot_size * slot_num);

        kdebug("slab memory chunk size : 0x%lx, num : %d\n",
               current_slot_size,
               slot_num);
    }

    return total_size;
}
