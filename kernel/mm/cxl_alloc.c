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

extern struct phys_mem_pool *global_cxl_mem[];

/* Declaration */
void *get_cxl_pages(int order)
{
#if TRACK_THREAD_MM == ON
        if (current_thread)
                current_thread->mm_size += (BUDDY_PAGE_SIZE * (1 << order));
#endif
        struct page *page = NULL;
        int i;

        /* Try to get continous physical memory pages from one physmem pool. */
        for (i = 0; i < physmem_map_num; ++i) {
                page = buddy_get_pages(global_cxl_mem[i], order);
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

void free_cxl_pages(void *addr)
{
        struct page *page;

        page = virt_to_page(addr);
        buddy_free_pages(page->pool, page);
}

static int size_to_page_order(unsigned long size)
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

/* Currently, BUG_ON no available memory. */
void *cxl_kmalloc(size_t size)
{
#if !defined USE_NVM || !defined USE_cxl
        return kmalloc(size);
#endif
        void *addr;
        int order;

        if (unlikely(size == 0))
                return ZERO_SIZE_PTR;

        if (size <= SLAB_MAX_SIZE) {
#if TRACK_THREAD_MM == ON
                if (current_thread)
                        current_thread->mm_size +=
                                (1 << size_to_slab_order(size));
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
        return addr;
}

void *cxl_kzalloc(size_t size)
{
        void *addr;

        addr = cxl_kmalloc(size);
        memset(addr, 0, size);
        return addr;
}

void cxl_kfree(void *ptr)
{
#if !defined USE_NVM || !defined USE_cxl
        kfree(ptr);
        return;
#endif
        struct page *page;

        if (unlikely(ptr == ZERO_SIZE_PTR))
                return;

        page = virt_to_page(ptr);
        if (page && page->slab) {
#if TRACK_THREAD_MM == ON
                if (current_thread)
                        current_thread->mm_size -=
                                (1
                                 << ((struct slab_header *)(page->slab))->order);
#endif
                free_in_slab(ptr);
        } else {
                int old_refcnt = atomic_fetch_sub_64(&page->ref_cnt, 1);
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
