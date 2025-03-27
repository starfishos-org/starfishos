#include "mm/kmalloc.h"
#include <common/types.h>
#include <common/macro.h>
#include <common/errno.h>
#include <common/util.h>
#include <common/kprint.h>
#include <arch/sync.h>
#if TRACK_THREAD_MM == ON
#include <object/thread.h>
#endif

#include <mm/slab.h>
#include <mm/buddy.h>

#define SLAB_MAX_SIZE (1UL << SLAB_MAX_ORDER)
#define ZERO_SIZE_PTR ((void *)(-1UL))

int size_to_page_order(unsigned long size)
{
    unsigned long order;
    unsigned long pg_num;
    unsigned long tmp;

    order = 0;
    pg_num = ROUND_UP(size, BUDDY_PAGE_SIZE) / BUDDY_PAGE_SIZE;
    tmp = pg_num;

    while (tmp > 1) {
        tmp >>= 1;
        order += 1;
    }

    if (pg_num > (1 << order))
        order += 1;

    return (int)order;
}

void free_pages(void *addr)
{
    struct page *page;

    page = virt_to_page(addr);
    buddy_free_pages(page->pool, page);
}

void kfree(void *ptr)
{
    struct page *page;
    int old_refcnt;

    if (unlikely(ptr == ZERO_SIZE_PTR))
        return;

    page = virt_to_page(ptr);
    if (page && page->slab) {
#if TRACK_THREAD_MM == ON
        if (current_thread)
            current_thread->mm_size -=
                    (1 << ((struct slab_header *)(page->slab))->order);
#endif
        switch (page->pool->type) {
        case DRAM_PAGE:
            free_in_dram_slab(ptr);
            break;
        case CXL_MEM_PAGE:
            free_in_cxl_slab(ptr);
            break;
        default:
            BUG("type %d currently not supported\n", page->pool->type);
        }
    } else {
        old_refcnt = atomic_fetch_sub_64(&page->ref_cnt, 1);
        if (old_refcnt == 1) {
#if TRACK_THREAD_MM == ON
            if (current_thread)
                current_thread->mm_size -=
                        (BUDDY_PAGE_SIZE * (1 << page->order));
#endif
            buddy_free_pages(page->pool, page);
        }
    }
}

#if 0
/* Currently, BUG_ON no available memory. */
void *kmalloc(size_t size)
{
        void *addr;
        int order;

        if (unlikely(size == 0))
                return ZERO_SIZE_PTR;

        if (size <= SLAB_MAX_SIZE) {
#if TRACK_THREAD_MM == ON
                if (current_thread)
                        current_thread->mm_size +=
                                (1 << size_to_page_order(size));
#endif
                addr = alloc_in_slab(size);
        } else {
                if (size <= BUDDY_PAGE_SIZE)
                        order = 0;
                else
                        order = size_to_page_order(size);
                addr = get_pages(order, __MT_DEFAULT__);
        }

        BUG_ON(!addr);
        return addr;
}

void *kzalloc(size_t size)
{
        void *addr;

        addr = kmalloc(size, __MT_DEFAULT__);
        memset(addr, 0, size);
        return addr;
}
#else
void *kmalloc(unsigned long long size, mem_t flags)
{
    BUG_ON(!IS_VALID_MEM_TYPE(flags));
#ifdef DSM_MALLOC_MODE_DRAM
    UNUSED(flags);
    return dram_kmalloc(size);
#elif defined(DSM_MALLOC_MODE_CXL)
    UNUSED(flags);
    return cxl_kmalloc(size);
#elif defined(DSM_MALLOC_MODE_MIXED_DEFAULT_DRAM) || defined(DSM_MALLOC_MODE_MIXED_DEFAULT_CXL)
    switch (flags) {
    case __MT_PRIVATE__:
        return dram_kmalloc(size);
    case __MT_SHARED__:
        return cxl_kmalloc(size);
    default:
        kwarn_once("%s: type: %d is not supported\n", __func__, flags);
    }
#else
#error "DSM_MALLOC_MODE must be defined"
#endif
    return NULL;
}

void *kzalloc(unsigned long long size, mem_t flags)
{
    BUG_ON(!IS_VALID_MEM_TYPE(flags));
    void *addr;

    addr = kmalloc(size, flags);
    memset(addr, 0, size);
    return addr;
}

/* Return vaddr of (1 << order) continous free physical pages */
void *get_pages(int order, mem_t flags)
{
    BUG_ON(!IS_VALID_MEM_TYPE(flags));
#ifdef DSM_MALLOC_MODE_DRAM
    UNUSED(flags);
    return get_dram_pages(order);
#elif defined(DSM_MALLOC_MODE_CXL)
    UNUSED(flags);
    return get_cxl_pages(order);
#elif defined(DSM_MALLOC_MODE_MIXED_DEFAULT_DRAM) || defined(DSM_MALLOC_MODE_MIXED_DEFAULT_CXL)
    switch (flags) {
    case __MT_PRIVATE__:
        return get_dram_pages(order);
    case __MT_SHARED__:
        return get_cxl_pages(order);
    default:
        kwarn_once("%s: type: %d is not supported\n", __func__, flags);
    }
#else
#error "DSM_MALLOC_MODE must be defined"
#endif
    return NULL;
}

#endif
