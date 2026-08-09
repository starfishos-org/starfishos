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
#ifdef DSM_ENABLED
#include <dsm/cxl_reclaim.h>
#endif

#define SLAB_MAX_SIZE (1UL << SLAB_MAX_ORDER)
#define ZERO_SIZE_PTR ((void *)(-1UL))

extern struct phys_mem_pool *global_cxl_mem[];
extern int cxlmem_map_num;

/* Declaration */
void *get_cxl_pages(int order)
{
#if TRACK_THREAD_MM == ON
    if (current_thread)
        current_thread->mm_size += (BUDDY_PAGE_SIZE * (1 << order));
#endif
    struct page *page = NULL;
    u64 requested_pages = 1UL << order;
    int i;

#ifdef DSM_ENABLED
    /*
     * Account the allocation, but never drive demotion from here.
     *
     * This allocator is reached from contexts that already hold mm locks --
     * most importantly map_page_in_pgtbl(), which allocates a page-table page
     * (__MT_PGTABLE__ resolves to __MT_SHARED__ when DSM_PGTABLE_MODE=CXL)
     * while holding vmspace->pgtbl_lock.  The demoter has to take
     * vmspace_lock and pgtbl_lock of the vmspaces it walks, and it issues
     * blocking cross-machine RPCs whose remote handlers take pgtbl_lock too.
     * Running it here therefore re-enters a non-recursive ticket lock on the
     * same CPU and wedges the whole cluster, because the vmspace and its page
     * tables are shared through CXL.
     *
     * dsm_cxl_reserve_resident_pages() only publishes demand when a migrated
     * user page crosses the soft residency threshold. A separate
     * polling-service worker consumes that demand through a bounded syscall
     * with no mm lock held. A failed allocator reservation retains the
     * pre-reclaim behaviour of reporting OOM to the caller.
     */
    if (dsm_cxl_reserve_pages(requested_pages) < 0) {
        kwarn("[OOM] Cannot reserve CXL pages!\n");
        return NULL;
    }
#endif

    /* Try to get continous physical memory pages from one physmem pool. */
    for (i = 0; i < cxlmem_map_num; ++i) {
        page = buddy_get_pages(global_cxl_mem[i], order);
        if (page)
            break;
    }

    if (unlikely(!page)) {
#ifdef DSM_ENABLED
        dsm_cxl_cancel_reserved_pages(requested_pages);
#endif
        kwarn("[OOM] Cannot get page from any memory pool!\n");
        return NULL;
    }

    /* Init page reference count */
    page->ref_cnt = 1;
#ifdef DSM_ENABLED
    dsm_cxl_commit_reserved_pages(requested_pages);
#endif

    return page_to_virt(page);
}

void free_cxl_pages(void *addr)
{
    struct page *page;

    page = virt_to_page(addr);
#ifdef DSM_ENABLED
    dsm_cxl_free_page(page);
#else
    buddy_free_pages(page->pool, page);
#endif
}

/* Currently, BUG_ON no available memory. */
void *cxl_kmalloc(size_t size)
{
    void *addr;
    int order;

    // kinfo("before cxl malloc: size: %ld\n", size);

    if (unlikely(size == 0))
        return ZERO_SIZE_PTR;

    if (size <= SLAB_MAX_SIZE) {
#if TRACK_THREAD_MM == ON
        if (current_thread)
            current_thread->mm_size += (1 << size_to_slab_order(size));
#endif
        addr = alloc_in_cxl_slab(size);
    } else {
        if (size <= BUDDY_PAGE_SIZE)
            order = 0;
        else
            order = size_to_page_order(size);
        addr = get_cxl_pages(order);
    }

    BUG_ON(!addr);
    // kinfo("cxl malloc: %p, size: %ld\n", addr, size);
    return addr;
}

void *cxl_kzalloc(size_t size)
{
    void *addr;

    addr = cxl_kmalloc(size);
    memset(addr, 0, size);
    return addr;
}
