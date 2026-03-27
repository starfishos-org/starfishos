#include <common/macro.h>
#include <common/types.h>
#include <common/kprint.h>
#include <common/lock.h>
#include <mm/kmalloc.h>
#include <mm/slab.h>
#include <mm/buddy.h>
#include <mm/nvm.h>
#ifdef DSM_ENABLED
#include <dsm/dsm-single.h>
#endif

/* Per-CPU CXL slab pools/locks, indexed by [cpu][order]. */
struct slab_pointer cxl_slab_pool[PLAT_CPU_NUM][SLAB_MAX_ORDER + 1];
static struct lock cxl_slabs_locks[PLAT_CPU_NUM][SLAB_MAX_ORDER + 1];

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

    next_slot = free_list->next_free;
    current_slab->free_list_head = next_slot;

    current_slab->current_free_cnt -= 1;
    /* When current_slab is full, choose a new slab as the current one. */
    if (unlikely(current_slab->current_free_cnt == 0))
        choose_new_current_slab(&cxl_slab_pool[cpu_id][order], order);

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
        }
    }
    kdebug("mm: finish initing slab allocators\n");
}

void *alloc_in_cxl_slab(unsigned long size)
{
    int order;

    BUG_ON(size > order_to_size(SLAB_MAX_ORDER));

    order = (int)size_to_order(size);
    if (order < SLAB_MIN_ORDER)
        order = SLAB_MIN_ORDER;

    return alloc_in_cxl_slab_impl(order);
}

void free_in_cxl_slab(void *addr)
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

    slot->next_free = slab->free_list_head;
    slab->free_list_head = slot;
    slab->current_free_cnt += 1;

    try_return_slab_to_buddy(slab, order);

#if CHECK_FREE_COUNT_IN_SLAB == ON
    check_slot_free_count(slab);
#endif

    unlock(&cxl_slabs_locks[owner_cpu][order]);
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
