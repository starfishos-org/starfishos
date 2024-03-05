#pragma once

#include <common/types.h>
#include <common/list.h>
#include <common/lock.h>

/* The following two are defined in mm.c and filled by mmparse.c. */
extern paddr_t physmem_map[][2];
extern int physmem_map_num;

extern paddr_t nvmmem_map[][2];
extern int nvmmem_map_num;

#ifdef USE_CXL_MEM
extern paddr_t cxlmem_map[][2];
extern int cxlmem_map_num;
#endif

/* All the following symbols are only used locally in the mm module. */

/* page flags field */
// #define PG_ALLOCATED    (1 << 0)        /* page is allocated */
// #define PG_ACTIVE       (1 << 1)        /* page is in active list */
// #define PG_CACHED       (1 << 2)        /* page is reserved */
// #define PG_PATCHED      (1 << 3)        /* page has patch */
enum pageflags {
        PG_allocated = 0, /* page is allocated */
        // PG_active,              /* page is in active list */
        PG_cached, /* page is moved from NVM to DRAM */
        PG_patched, /* page has patch */
        PG_flagnum,
};

enum page_type {
        DRAM_PAGE = 0,
        DRAM_CACHED_PAGE,
        NVM_PAGE,
        CXL_MEM_PAGE, /* page allocated from CXL Fixed Memory Window */
        INVALID_PAGE,
};

typedef u16 page_type_t;

/* `struct page` is the metadata of one physical 4k page. */
struct page {
        /* Free list */
        struct list_head node;
        /* Flags store information of correspond physical page. */
        u32 flags;
        /* The order of the memory chunck that this page belongs to. */
        int order;
        /* Used for ChCore slab allocator. */
        void *slab;
        /* The physical memory pool this page belongs to */
        struct phys_mem_pool *pool;
        /* The lock for page */
        struct lock lock;
        /* Reference count for ChCore fork */
        int ref_cnt;
#ifdef CHCORE_SLS
#ifdef PMO_CHECKSUM
        u64 ckpt_version_number;
#endif
#ifdef RMAP_ENABLED
        /* PMO page belongs to and index in PMO */
        struct pmobject *pmo;
        u64 index;
        /* The head of page in a contious page list */
        u64 compound_head;
#endif
        u64 page_pair;
#ifdef HYBRID_MEM
        /* Page track info */
        struct page_track_info *track_info;
#endif
#endif
};

struct free_list {
        struct list_head free_list;
        unsigned long nr_free;
};

enum log_commit_type { LOG_INIT = 0, LOG_DONE = 1, LOG_COMMIT = 2 };
typedef u8 log_commit_type_t;

enum log_type { ADD_PAGES = 0, REMOVE_PAGES = 1 };
typedef u8 log_type_t;

struct log_entry {
        log_commit_type_t commited;
        log_type_t type;
        u64 page;
        u32 dedicated_order;
        u32 cur_order;
        u64 list_cur_num;
};

/*
 * Supported Order: [0, BUDDY_MAX_ORDER).
 * The max allocated size (continous physical memory size) is
 * 2^(BUDDY_MAX_ORDER - 1) * 4K.
 * Given BUDDY_MAX_ORDER is 14, the max allocated chunk is 32M.
 */
#define BUDDY_PAGE_SIZE (0x1000)
#define BUDDY_MAX_ORDER (18UL)

/* One page size is 4K, so the order is 12. */
#define BUDDY_PAGE_SIZE_ORDER (12)

/* Each physical memory chunk can be represented by one physical memory pool. */
struct phys_mem_pool {
        /*
         * The start virtual address (for used in kernel) of
         * this physical memory pool.
         */
        vaddr_t pool_start_addr;
        unsigned long pool_mem_size;

        /*
         * The start virtual address (for used in kernel) of
         * the metadata area of this pool.
         */
        struct page *page_metadata;

        /* One lock for one pool. */
        struct lock buddy_lock;

        /* The free list of different free-memory-chunk orders. */
        struct free_list free_lists[BUDDY_MAX_ORDER];

        /*
         * This field is only used in ChCore unit test.
         * The number of (4k) physical pages in this physical memory pool.
         */
        unsigned long pool_phys_page_num;

        /* Type of mem pool */
        page_type_t type;
#ifdef CHCORE_SLS
        /* Logs of the latest log */
        struct log_entry latest_log;
#endif
};

/* Disjoint physical memory can be represented by several phys_mem_pools. */
extern struct phys_mem_pool *global_mem[];
extern struct phys_mem_pool *global_dram_mem[];

#ifdef USE_CXL_MEM
extern struct phys_mem_pool *global_cxl_mem[];
#endif

#ifdef DSM_LINEAR_MM_LAYOUT
extern struct phys_mem_pool *global_temp_mem;
#endif

/* All interfaces are kernel/mm module internal interfaces. */

void init_buddy(struct phys_mem_pool *, struct page *start_page,
                vaddr_t start_addr, unsigned long page_num, page_type_t type);

struct page *buddy_get_pages(struct phys_mem_pool *, int order);
void buddy_free_pages(struct phys_mem_pool *, struct page *page);

void *page_to_virt(struct page *page);
struct page *virt_to_page(void *ptr);
unsigned long get_free_mem_size_from_buddy(struct phys_mem_pool *);

/* get page type by virt addr of page */
page_type_t get_page_type(struct page *page);

#ifdef CHCORE_SLS
/* latest log related  */
void prepare_latest_log(struct phys_mem_pool *pool, log_type_t type, u64 page,
                        u32 dedicated_order, u32 cur_order);
void commit_latest_log(struct phys_mem_pool *);
void apply_latest_log(struct phys_mem_pool *);
#endif /* CHCORE_SLS */

/* set/clear flags of page */
static inline void page_set_flag(struct page *page, u32 flag)
{
        BUG_ON(flag >= PG_flagnum);
        page->flags |= (1 << flag);
}

static inline void page_clear_flag(struct page *page, u32 flag)
{
        BUG_ON(flag >= PG_flagnum);
        page->flags &= ~(1 << flag);
}

static inline u32 page_check_flag(struct page *page, u32 flag)
{
        BUG_ON(flag >= PG_flagnum);
        return (page->flags & (1 << flag));
}

#ifdef RMAP_ENABLED
static inline struct page *compound_head(struct page *page)
{
        u64 head = page->compound_head;

        if (unlikely(head & 1))
                return (struct page *)(head - 1);
        return page;
}

static inline void set_compound_head(struct page *page, struct page *head)
{
        BUG_ON(((u64)page - (u64)head) % sizeof(struct page));
        page->compound_head = (u64)head + 1;
}

static inline void clear_compound_head(struct page *page)
{
        page->compound_head = 0;
}

static inline u64 compound_head_offset(struct page *page, struct page *head)
{
        u64 dis = (u64)page - (u64)head;
        BUG_ON(dis % sizeof(struct page));
        return dis / sizeof(struct page);
}
#endif

/* TreeSlS */
#ifdef CHCORE_SLS
static inline void init_page_info(struct page *page, struct pmobject *pmo,
                                  u64 index)
{
        page->index = index;
        page->pmo = pmo;
        page->page_pair = 0;
}
#endif
