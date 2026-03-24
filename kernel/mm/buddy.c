#include <common/util.h>
#include <common/macro.h>
#include <common/kprint.h>
#include <mm/buddy.h>
#include <mm/rmap.h>
#ifdef DSM_ENABLED
#include <dsm/dsm-single.h>
#include <arch/mmu.h>
#endif

static struct page *get_buddy_chunk(struct phys_mem_pool *pool,
                                    struct page *chunk)
{
    vaddr_t chunk_addr;
    vaddr_t buddy_chunk_addr;
    int order;

    /* Get the address of the chunk. */
    chunk_addr = (vaddr_t)page_to_virt(chunk);
    order = chunk->order;
    /*
     * Calculate the address of the buddy chunk according to the address
     * relationship between buddies.
     */
    buddy_chunk_addr = chunk_addr ^ (1UL << (order + BUDDY_PAGE_SIZE_ORDER));

    /* Check whether the buddy_chunk_addr belongs to pool. */
    if ((buddy_chunk_addr < pool->pool_start_addr)
        || ((buddy_chunk_addr + (1 << order) * BUDDY_PAGE_SIZE)
            > (pool->pool_start_addr + pool->pool_mem_size))) {
        return NULL;
    }

    return virt_to_page((void *)buddy_chunk_addr);
}

/* The most recursion level of split_chunk is decided by the macro of
 * BUDDY_MAX_ORDER. */
static struct page *split_chunk(struct phys_mem_pool *pool, int order,
                                struct page *chunk)
{
    struct page *buddy_chunk;
    struct list_head *free_list;

    /*
     * If the @chunk's order equals to the required order,
     * return this chunk.
     */
    if (chunk->order == order)
        return chunk;

    /*
     * If the current order is larger than the required order,
     * split the memory chunck into two halves.
     */
    chunk->order -= 1;

    buddy_chunk = get_buddy_chunk(pool, chunk);
    /* The buddy_chunk must exist since we are spliting a large chunk. */
    BUG_ON(buddy_chunk == NULL);

    /* Set the metadata of the remaining buddy_chunk. */
    buddy_chunk->order = chunk->order;
    page_clear_flag(buddy_chunk, PG_allocated);

    /* Put the remaining buddy_chunk into its correspondint free list. */
    free_list = &(pool->free_lists[buddy_chunk->order].free_list);
    list_add(&buddy_chunk->node, free_list);
    pool->free_lists[buddy_chunk->order].nr_free += 1;

    /* Continue to split current chunk (@chunk). */
    return split_chunk(pool, order, chunk);
}

/* The most recursion level of merge_chunk is decided by the macro of
 * BUDDY_MAX_ORDER. */
static struct page *merge_chunk(struct phys_mem_pool *pool, struct page *chunk)
{
    struct page *buddy_chunk;

    /* The @chunk has already been the largest one. */
    if (chunk->order == (BUDDY_MAX_ORDER - 1)) {
        return chunk;
    }

    /* Locate the buddy_chunk of @chunk. */
    buddy_chunk = get_buddy_chunk(pool, chunk);

    /* If the buddy_chunk does not exist, no further merge is required. */
    if (buddy_chunk == NULL)
        return chunk;

    /* The buddy_chunk is not free, no further merge is required. */
    if (page_check_flag(buddy_chunk, PG_allocated))
        return chunk;

    /* The buddy_chunk is not free as a whole, no further merge is required.
     */
    if (buddy_chunk->order != chunk->order)
        return chunk;

    /* Remove the buddy_chunk from its current free list. */
    list_del(&(buddy_chunk->node));
    pool->free_lists[buddy_chunk->order].nr_free -= 1;

    /* Merge the two buddies and get a larger chunk @chunk (order+1). */
    buddy_chunk->order += 1;
    chunk->order += 1;
    if (chunk > buddy_chunk)
        chunk = buddy_chunk;

    /* Keeping merging. */
    return merge_chunk(pool, chunk);
}

/*
 * The layout of a phys_mem_pool:
 * | page_metadata are (an array of struct page) | alignment pad | usable memory
 * |
 *
 * The usable memory: [pool_start_addr, pool_start_addr + pool_mem_size).
 */
void init_buddy(struct phys_mem_pool *pool, struct page *start_page,
                vaddr_t start_addr, unsigned long page_num, page_type_t type)
{
    int order;
    int page_idx;
    struct page *page;

    BUG_ON(lock_init(&pool->buddy_lock) != 0);

    /* Init the physical memory pool. */
    pool->pool_start_addr = start_addr;
    pool->page_metadata = start_page;
    pool->pool_mem_size = page_num * BUDDY_PAGE_SIZE;
    /* This field is for unit test only. */
    pool->pool_phys_page_num = page_num;
    pool->type = type;

    kinfo("[BUDDY_INIT] pool: %p, type: %d, pool_start_addr: 0x%lx, pool_mem_size: 0x%lx, page_num: %lu"
#ifdef DSM_ENABLED
          ", machine_id: %d"
#endif
          "\n",
          pool, type, pool->pool_start_addr, pool->pool_mem_size, page_num
#ifdef DSM_ENABLED
          , CUR_MACHINE_ID
#endif
    );

    /* Init the free lists */
    for (order = 0; order < BUDDY_MAX_ORDER; ++order) {
        pool->free_lists[order].nr_free = 0;
        init_list_head(&(pool->free_lists[order].free_list));
    }

    /* Clear the page_metadata area. */
    memset((char *)start_page, 0, page_num * sizeof(struct page));
    // printk("buddy system page area end: 0x%llx\n", (u64)start_page + page_num
    // * sizeof(struct page));

    /* Init the page_metadata area. */
    for (page_idx = 0; page_idx < page_num; ++page_idx) {
        page = start_page + page_idx;
        page_set_flag(page, PG_allocated);
        page->order = 0;
        page->pool = pool;
#ifdef RMAP_ENABLED
        page->pmo = NULL;
        page->index = 0;
#endif
#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
        page->page_pair = 0;
#endif
    }

    /* Put each physical memory page into the free lists. */
    for (page_idx = 0; page_idx < page_num; ++page_idx) {
        page = start_page + page_idx;
        buddy_free_pages(pool, page);
    }
}

struct page *buddy_get_pages(struct phys_mem_pool *pool, int order)
{
#if defined(USE_CXL_MEM) && defined(DSM_CXL_LF_BUDDY)
    if (pool->type == CXL_MEM_PAGE)
        return buddy_lf_get_pages(pool, order);
#endif
    int cur_order;
    struct list_head *free_list;
    struct page *page = NULL;

    if (unlikely(order >= BUDDY_MAX_ORDER)) {
        kwarn("ChCore does not support allocating such too large "
              "contious physical memory\n");
        return NULL;
    }

    lock(&pool->buddy_lock);

    /* Search a chunk (with just enough size) in the free lists. */
    for (cur_order = order; cur_order < BUDDY_MAX_ORDER; ++cur_order) {
        free_list = &(pool->free_lists[cur_order].free_list);
        if (!list_empty(free_list)) {
            /* Get a free memory chunck from the free list */
            page = list_entry(free_list->next, struct page, node);
#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
            prepare_latest_log(pool, ADD_PAGES, (u64)page, order, cur_order);
#endif
            list_del(&page->node);
            pool->free_lists[cur_order].nr_free -= 1;
            break;
        }
    }

    if (unlikely(page == NULL)) {
        kwarn("[OOM] No enough memory in memory pool %p\n", pool);
        goto out;
    }

    /*
     * Split the chunk found and return the start part of the chunck
     * which can meet the required size.
     */
    page = split_chunk(pool, order, page);

    /* Set information of pages followed by head */
    for (int i = 0; i < (1 << order); i++) {
        struct page *p = page + i;
#ifdef RMAP_ENABLED
        set_compound_head(p, page);
#endif
        page_set_flag(p, PG_allocated);
    }

out:
#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
    commit_latest_log(pool);
#endif
    unlock(&pool->buddy_lock);
    return page;
}

extern void destory_track_info(struct page *page);

void buddy_free_pages(struct phys_mem_pool *pool, struct page *page)
{
#if defined(USE_CXL_MEM) && defined(DSM_CXL_LF_BUDDY)
    if (pool->type == CXL_MEM_PAGE) {
        buddy_lf_free_pages(pool, page);
        return;
    }
#endif
    int order, i;
    struct list_head *free_list;
    struct page *p;

    lock(&pool->buddy_lock);

#if defined CHCORE_SLS
    prepare_latest_log(pool, REMOVE_PAGES, (u64)page, page->order, 0);
#endif
    for (i = 0; i < (1 << page->order); i++) {
        p = page + i;
        BUG_ON(!page_check_flag(p, PG_allocated));
        /* Clear all flags of page */
        p->flags = 0;
#if defined CHCORE_SLS
#ifdef RMAP_ENABLED
        /* Clear information of pages followed by head */
        clear_compound_head(p);
        /* clear pmo and index */
        p->pmo = NULL;
        p->index = 0;
#endif
        p->page_pair = 0;
#endif
    }
#if defined(CHCORE_SLS) && defined(HYBRID_MEM)
    /* Mark @page's track info as NULL and remove from active list */
    if (page->track_info)
        destory_track_info(page);
#endif
    /* Merge the freed chunk. */
    page = merge_chunk(pool, page);

    /* Put the merged chunk into the its corresponding free list. */
    order = page->order;
    free_list = &(pool->free_lists[order].free_list);
    list_add(&page->node, free_list);
    pool->free_lists[order].nr_free += 1;

#if defined CHCORE_SLS
    commit_latest_log(pool);
#endif
    unlock(&pool->buddy_lock);
}

void *page_to_virt(struct page *page)
{
    vaddr_t addr;
    struct phys_mem_pool *pool = page->pool;

    BUG_ON(pool == NULL);

    /* page_idx * BUDDY_PAGE_SIZE + start_addr */
    addr = (page - pool->page_metadata) * BUDDY_PAGE_SIZE
           + pool->pool_start_addr;
    BUG_ON(addr > pool->pool_start_addr + pool->pool_mem_size);
    return (void *)addr;
}

struct page *virt_to_page(void *ptr)
{
    struct page *page;
    struct phys_mem_pool *pool = NULL;
    vaddr_t addr = (vaddr_t)ptr;
    int i;
#if 0
        /* Find the corresponding physical memory pool. */
        for (i = 0; i < physmem_map_num; ++i) {
                if (addr >= global_mem[i]->pool_start_addr
                    && addr < global_mem[i]->pool_start_addr
                                       + global_mem[i]->pool_mem_size) {
                        pool = global_mem[i];
                        break;
                }
        }
        if (pool)
                goto out;
#endif
#ifdef USE_DRAM
    for (i = 0; i < physmem_map_num; ++i) {
        if (addr >= global_dram_mem[i]->pool_start_addr
            && addr < global_dram_mem[i]->pool_start_addr
                               + global_dram_mem[i]->pool_mem_size) {
            pool = global_dram_mem[i];
            break;
        }
    }
    if (pool)
        goto find;
#endif
#ifdef USE_CXL_MEM
    for (i = 0; i < cxlmem_map_num; ++i) {
        if (addr >= global_cxl_mem[i]->pool_start_addr
            && addr < global_cxl_mem[i]->pool_start_addr
                               + global_cxl_mem[i]->pool_mem_size) {
            pool = global_cxl_mem[i];
            break;
        }
    }
    if (pool)
        goto find;
#endif
#ifdef DSM_LINEAR_MM_LAYOUT
    if (addr >= global_temp_mem->pool_start_addr
        && addr < global_temp_mem->pool_start_addr
                           + global_temp_mem->pool_mem_size) {
        pool = global_temp_mem;
        goto find;
    }
#endif
    BUG("pool=NULL for va=%llx\n", addr);

find:
    page = pool->page_metadata
           + (((vaddr_t)addr - pool->pool_start_addr) / BUDDY_PAGE_SIZE);
    BUG_ON(page->pool != pool);
    return page;
}

page_type_t get_page_type(struct page *page)
{
    if (page_check_flag(page, PG_cached))
        return DRAM_CACHED_PAGE;

    /* Find the corresponding global memory pool. */
    if (page->pool)
        return page->pool->type;
    /* does not belong to memory pool */
    return INVALID_PAGE;
}

/* Get machine ID for a given physical address, or -1 if it's shared memory (CXL) or not found */
#ifdef DSM_ENABLED
int get_paddr_machine_id(paddr_t paddr)
{
#if defined DSM_MALLOC_MODE_TEMP || defined DSM_MALLOC_MODE_DRAM
    // If we default use DRAM and TEMP, all pages are shared memory
    return MACHINE_ID_SHARED_MEMORY;
#endif
    /* Check if it's shared memory (CXL) */
    if (IS_SHM_PADDR(paddr))
        return MACHINE_ID_SHARED_MEMORY; /* Shared memory, no specific machine */
    
    /* Check each machine's local memory range */
    for (int i = 0; i < CLUSTER_MACHINE_NUM; i++) {
        if (IS_LOCAL_PADDR(paddr, i))
            return i;
    }
    
    return MACHINE_ID_INVALID; /* Not found */
}
#else
/* Single machine system, always return 0 */
int get_paddr_machine_id(paddr_t paddr)
{
    UNUSED(paddr);
    return MACHINE_ID_SHARED_MEMORY;
}
#endif

/* Get machine ID for a given page, or -1 if it's shared memory (CXL) or not found */
int get_page_machine_id(struct page *page)
{
    paddr_t paddr;
    
    if (!page || !page->pool)
        return MACHINE_ID_INVALID;
    
    /* Get physical address from page */
    paddr = virt_to_phys(page_to_virt(page));
    
    /* Use the paddr-based function */
    return get_paddr_machine_id(paddr);
}

unsigned long get_free_mem_size_from_buddy(struct phys_mem_pool *pool)
{
    int order;
    struct free_list *list;
    unsigned long current_order_size;
    unsigned long total_size = 0;

    for (order = 0; order < BUDDY_MAX_ORDER; order++) {
        /* 2^order * 4K */
        current_order_size = BUDDY_PAGE_SIZE * (1 << order);
        list = pool->free_lists + order;
        total_size += list->nr_free * current_order_size;

        /* debug : print info about current order */
        kdebug("buddy memory chunk order: %d, size: 0x%lx, num: %d\n",
               order,
               current_order_size,
               list->nr_free);
    }
    return total_size;
}

#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
void prepare_latest_log(struct phys_mem_pool *pool, log_type_t type, u64 page,
                        u32 dedicated_order, u32 cur_order)
{
    struct log_entry *log = &(pool->latest_log);
    log->type = type;
    log->page = page;
    log->dedicated_order = dedicated_order;
    log->cur_order = cur_order;
    log->list_cur_num = pool->free_lists[cur_order].nr_free;
    // smp_mb();
    log->commited = LOG_DONE;
}

void commit_latest_log(struct phys_mem_pool *pool)
{
    pool->latest_log.commited = LOG_COMMIT;
    // smp_mb();
}

void recover_nr_free(struct free_list *free_list)
{
    struct page *iter;
    free_list->nr_free = 0;
    for_each_in_list (iter, struct page, node, &(free_list->free_list)) {
        free_list->nr_free += 1;
    }
}

static struct page *merge_chunk2(struct phys_mem_pool *pool, struct page *chunk,
                                 u32 target_order)
{
    struct page *buddy_chunk;
    struct free_list *free_list;
    struct page *iter_forward, *iter_backward;

    if (chunk->order == target_order) {
        return chunk;
    }

    /* Locate the buddy_chunk of @chunk. */
    buddy_chunk = get_buddy_chunk(pool, chunk);

    if (buddy_chunk == NULL) {
        BUG("%s: buddy chunk cannot be found.\n", __func__);
    }

    free_list = &(pool->free_lists[chunk->order]);

    /* Remove the buddy_chunk from its current free list. */
    /* Handle a partly executed list_add() */
    for_each_in_list (
            iter_forward, struct page, node, &(free_list->free_list)) {
        if (iter_forward == buddy_chunk) {
            break;
        }
    }

    for_each_in_list_backward(
            iter_backward, struct page, node, &(free_list->free_list))
    {
        if (iter_backward == buddy_chunk) {
            break;
        }
    }

    if (iter_forward == buddy_chunk || iter_backward == buddy_chunk) {
        list_del(&buddy_chunk->node);
    }

    /* Recover nr_free */
    recover_nr_free(free_list);

    /* Merge the two buddies and get a larger chunk @chunk (order+1). */
    chunk->order += 1;
    buddy_chunk->order = chunk->order;

    if (chunk > buddy_chunk)
        chunk = buddy_chunk;

    /* Keeping merging. */
    return merge_chunk2(pool, chunk, target_order);
}

static struct page *merge_chunk3(struct phys_mem_pool *pool, struct page *chunk)
{
    struct page *buddy_chunk;
    struct free_list *free_list;
    struct page *iter_forward, *iter_backward;

    /* The @chunk has already been the largest one. */
    if (chunk->order == (BUDDY_MAX_ORDER - 1)) {
        return chunk;
    }

    /* Locate the buddy_chunk of @chunk. */
    buddy_chunk = get_buddy_chunk(pool, chunk);

    /* If the buddy_chunk does not exist, no further merge is required. */
    if (buddy_chunk == NULL)
        return chunk;

    /* The buddy_chunk is not free, no further merge is required. */
    if (page_check_flag(buddy_chunk, PG_allocated))
        return chunk;

    /* The buddy_chunk is not free as a whole, no further merge is required.
     */
    if (buddy_chunk->order < chunk->order)
        return chunk;

    free_list = &(pool->free_lists[chunk->order]);

    /* Remove the buddy_chunk from its current free list. */
    /* handle a partly executed list_del() */
    for_each_in_list (
            iter_forward, struct page, node, &(free_list->free_list)) {
        if (iter_forward == buddy_chunk) {
            break;
        }
    }

    for_each_in_list_backward(
            iter_backward, struct page, node, &(free_list->free_list))
    {
        if (iter_backward == buddy_chunk) {
            break;
        }
    }

    if (iter_forward == buddy_chunk || iter_backward == buddy_chunk) {
        list_del(&buddy_chunk->node);
    }

    /* Recover nr_free */
    recover_nr_free(free_list);

    /* Merge the two buddies and get a larger chunk @chunk (order+1). */
    chunk->order += 1;
    buddy_chunk->order = chunk->order;

    if (chunk > buddy_chunk)
        chunk = buddy_chunk;

    /* Keeping merging. */
    return merge_chunk3(pool, chunk);
}

void undo_get_pages(struct phys_mem_pool *pool, struct log_entry *log)
{
    struct free_list *free_list = &(pool->free_lists[log->cur_order]);
    struct page *page = (struct page *)log->page;

    /* if the page has been splited */
    if (log->dedicated_order != log->cur_order) {
        page->order = log->dedicated_order;
        merge_chunk2(pool, page, log->cur_order);
    }
    BUG_ON(log->cur_order != page->order);
    /* reset the allocated page */
    for (int i = 0; i < (1 << page->order); i++) {
        struct page *p = page + i;
#ifdef RMAP_ENABLED
        clear_compound_head(p);
#endif
        page_clear_flag(p, PG_allocated);
    }
    /* add page to free list */
    if (free_list->free_list.next == &(page->node)) {
        /* page is still in free list, do nothing */
        /* case: failed just between prepare_log and
         * list_del(&page->node); */
    } else {
        /* handle a partly list_del */
        free_list->free_list.prev->next = &(free_list->free_list);
        /* add undoed chunk to list */
        list_add(&(page->node), &(free_list->free_list));
    }

    /* recover nr_free */
    recover_nr_free(free_list);
    BUG_ON(free_list->nr_free != log->list_cur_num);
}

void redo_free_pages(struct phys_mem_pool *pool, struct log_entry *log)
{
    int order, i;
    struct free_list *free_list;
    struct page *p;
    struct page *iter_forward, *iter_backward, *page;

    page = (struct page *)log->page;
    page->order = log->dedicated_order;

    /* redo page flags setting */
    for (i = 0; i < (1 << page->order); i++) {
        p = page + i;
        BUG_ON(!page_check_flag(p, PG_allocated));
        /* Clear all flags of page */
        p->flags = 0;
/* Clear information of pages followed by head */
#ifdef RMAP_ENABLED
        clear_compound_head(p);
#endif
        /* clear pmo and index */
        p->pmo = NULL;
        p->index = 0;
        p->page_pair = 0;
    }
/* Mark @page's track info as NULL and remove from active list */
#ifdef HYBRID_MEM
    if (page->track_info)
        destory_track_info(page);
#endif
    /* Merge the freed chunk. */
    page = merge_chunk3(pool, page);

    order = page->order;
    free_list = &(pool->free_lists[order]);

    /* handle a partly exectued list_add() */
    for_each_in_list (
            iter_forward, struct page, node, &(free_list->free_list)) {
        if (iter_forward == page) {
            break;
        }
    }

    for_each_in_list_backward(
            iter_backward, struct page, node, &(free_list->free_list))
    {
        if (iter_backward == page) {
            break;
        }
    }

    if (iter_forward == page || iter_backward == page) {
        list_del(&page->node);
    }

    /* Put the merged chunk into the its corresponding free list. */
    list_add(&page->node, &(free_list->free_list));

    /* recover nr_free */
    recover_nr_free(free_list);
}

void apply_latest_log(struct phys_mem_pool *pool)
{
    struct log_entry *log = &(pool->latest_log);
    switch (log->commited) {
    case LOG_INIT:
        /* initial state */
    case LOG_COMMIT:
        /* already commited */
        break;
    case LOG_DONE: {
        /* set log but not commited */
        if (log->type == ADD_PAGES) {
            /* undo get_pages */
            undo_get_pages(pool, log);
        } else if (log->type == REMOVE_PAGES) {
            /* redo free_pages */
            redo_free_pages(pool, log);
        } else {
            BUG("Invalid log type\n");
        }
    }
    default:
        BUG("Invalid log type\n");
        break;
    }
}
#endif /* CHCORE_SLS */
