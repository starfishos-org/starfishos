#include <common/types.h>
#include <common/macro.h>
#include <common/errno.h>
#include <common/util.h>
#include <common/kprint.h>
#include <arch/sync.h>
#include <mm/kmalloc.h>
#if TRACK_THREAD_MM == ON
#include <object/thread.h>
#endif

#include <mm/slab.h>
#include <mm/buddy.h>

#define SLAB_MAX_SIZE (1UL << SLAB_MAX_ORDER)
#define ZERO_SIZE_PTR ((void *)(-1UL))

extern struct phys_mem_pool *global_dram_mem[];

/* Declaration */
void *get_dram_pages(int order)
{
// #if !defined USE_NVM || !defined USE_DRAM
//         return get_pages(order, __DEFAULT__);
// #endif
#if TRACK_THREAD_MM == ON
    if (current_thread)
        current_thread->mm_size += (BUDDY_PAGE_SIZE * (1 << order));
#endif
    struct page *page = NULL;
    int i;

    /* Try to get continous physical memory pages from one physmem pool. */
    for (i = 0; i < physmem_map_num; ++i) {
        page = buddy_get_pages(global_dram_mem[i], order);
        if (page)
            break;
    }

    if (unlikely(!page)) {
        kwarn("[OOM] Cannot get page from any memory pool!\n");
        return NULL;
    }

    /* Init page reference count */
    page->ref_cnt = 1;

    return page_to_virt(page);
}

/* Currently, BUG_ON no available memory. */
void *dram_kmalloc(size_t size)
{
    void *addr;
    int order;

    if (unlikely(size == 0))
        return ZERO_SIZE_PTR;

    if (size <= SLAB_MAX_SIZE) {
#if TRACK_THREAD_MM == ON
        if (current_thread)
            current_thread->mm_size += (1 << size_to_slab_order(size));
#endif
        addr = alloc_in_dram_slab(size);
    } else {
        if (size <= BUDDY_PAGE_SIZE)
            order = 0;
        else
            order = size_to_page_order(size);
        addr = get_dram_pages(order);
    }
    // printk("%s: addr=%llx, size=%llx\n", __func__, addr, size);
    BUG_ON(!addr);
    return addr;
}
