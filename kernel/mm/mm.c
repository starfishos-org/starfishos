#include <common/types.h>
#include <mm/mm.h>
#include <common/kprint.h>
#include <common/macro.h>
#include <mm/slab.h>
#include <mm/buddy.h>
#include <mm/nvm.h>
#include <drivers/cxl.h>
#include <dsm/dsm-single.h>

extern void parse_mem_map(void *);

/* The following two will be filled by parse_mem_map. */
paddr_t physmem_map[N_PHYS_MEM_POOLS][2];
int physmem_map_num;

/* DRAM memory range (replacing physmem_map[0]) */
paddr_t dram_mem_start;
u64 dram_mem_size;

#ifdef DSM_LINEAR_MM_LAYOUT
/* A temparol memory pool for local info */
paddr_t temp_physmem_map[2];
int temp_physmem_map_num;

struct phys_mem_pool *global_temp_mem;
struct phys_mem_pool static_global_temp_mem;

/* Temp memory for global_temp_mem initialization (1G) */
paddr_t temp_mem_start;
u64 temp_mem_size;
#endif

#ifdef USE_NVM
extern void parse_nvm_map(void); /* Currently only support X86 */

/* The following two will be filled by parse_nvmmem_map. */
paddr_t nvmmem_map[N_PHYS_MEM_POOLS][2];
int nvmmem_map_num;
#endif /* USE_NVM */

#ifdef USE_CXL_MEM
extern void parse_cxlmem_map(void);

/* The following two will be filled by parse_cxlmem_map. */
paddr_t cxlmem_map[N_PHYS_MEM_POOLS][2];
int cxlmem_map_num;

struct phys_mem_pool *global_cxl_mem[N_PHYS_MEM_POOLS];
struct phys_mem_pool static_global_cxl_mem[N_PHYS_MEM_POOLS];
#endif /* USE_CXL_MEM */

struct phys_mem_pool *global_mem[N_PHYS_MEM_POOLS];
struct phys_mem_pool static_global_mem[N_PHYS_MEM_POOLS];

struct phys_mem_pool *global_dram_mem[N_PHYS_MEM_POOLS];
struct phys_mem_pool static_global_dram_mem[N_PHYS_MEM_POOLS];

#if defined(CHCORE_SLS) && !defined(USE_NVM)
static struct nvm_metadata nvm_metadata_for_dram;
#endif

#ifdef USE_NVM
static void init_buddy_for_one_nvmmem_map(int physmem_map_idx)
{
    paddr_t free_mem_start = 0;
    paddr_t free_mem_end = 0;
    struct page *page_meta_start = NULL;
    unsigned long npages = 0;
    unsigned long npages1 = 0;
    paddr_t free_page_start = 0;

    /* FIXME(FN): currently only use one NVM device */
    BUG_ON(physmem_map_idx != 0);

    /*
     * 		 GLOBAL_MEM_OFF          BUDDY_SYS_OFF
     *  V    		       V                      V
     *  struct nvm_metadata   struct phys_mem_pool   buddy system
     */
    free_mem_start = nvmmem_map[physmem_map_idx][0] + BUDDY_SYS_OFFSET;
    free_mem_end = nvmmem_map[physmem_map_idx][1];
    kdebug("mem pool %d, free_mem_start: 0x%lx, free_mem_end: 0x%lx\n",
           physmem_map_idx,
           free_mem_start,
           free_mem_end);

    global_mem[physmem_map_idx] =
            (struct phys_mem_pool *)((vaddr_t)nvm_metadata + GLOBAL_MEM_OFFSET);
    kdebug("global_mem[%d] start: %p\n",
           physmem_map_idx,
           global_mem[physmem_map_idx]);
#ifdef RESTORE_ENABLED
    if (NVM_IS_CRASH) {
        kinfo("[Restore] detech last crash and recover from it\n");
        return;
    }
#endif
    npages =
            (free_mem_end - free_mem_start) / (PAGE_SIZE + sizeof(struct page));
    free_page_start =
            ROUND_UP(free_mem_start + npages * sizeof(struct page), PAGE_SIZE);

    /* Recalculate npages after alignment. */
    npages1 = (free_mem_end - free_page_start) / PAGE_SIZE;
    npages = npages < npages1 ? npages : npages1;

    page_meta_start = (struct page *)phys_to_virt(free_mem_start);
    kdebug("page_meta_start: 0x%lx, npages: 0x%lx, meta_size: 0x%lx, free_page_start: 0x%lx\n",
           page_meta_start,
           npages,
           sizeof(struct page),
           free_page_start);

    /* Initialize the buddy allocator based on this free memory region. */
    init_buddy(global_mem[physmem_map_idx],
               page_meta_start,
               phys_to_virt(free_page_start),
               npages,
               NVM_PAGE);
}
#endif

/*
 * The layout of each physmem:
 * | metadata (npages * sizeof(struct page)) | start_vaddr ... (npages *
 * PAGE_SIZE) |
 */
void init_buddy_for_one_mem_pool(struct phys_mem_pool *pool, page_type_t type,
                                 paddr_t free_mem_start, paddr_t free_mem_end)
{
    struct page *page_meta_start = NULL;
    unsigned long npages = 0;
    unsigned long npages1 = 0;
    paddr_t free_page_start = 0;

    kdebug("mem pool type %d, free_mem_start: 0x%lx, free_mem_end: 0x%lx\n",
          type,
          free_mem_start,
          free_mem_end);

    npages =
            (free_mem_end - free_mem_start) / (PAGE_SIZE + sizeof(struct page));
    free_page_start =
            ROUND_UP(free_mem_start + npages * sizeof(struct page), PAGE_SIZE);

    /* Recalculate npages after alignment. */
    npages1 = (free_mem_end - free_page_start) / PAGE_SIZE;
    npages = npages < npages1 ? npages : npages1;

    page_meta_start = (struct page *)phys_to_virt(free_mem_start);
    kdebug("page_meta_start: 0x%lx, npages: 0x%lx, meta_size: 0x%lx, free_page_start: 0x%lx\n",
           page_meta_start,
           npages,
           sizeof(struct page),
           free_page_start);

    /* Initialize the buddy allocator based on this free memory region. */
    init_buddy(
            pool, page_meta_start, phys_to_virt(free_page_start), npages, type);
}

void dram_mm_init()
{
#ifdef USE_DRAM
    int i = 0;
    int physmem_map_idx;
    paddr_t free_mem_start = 0, free_mem_end = 0;

    /* Init dram pool */
    for (i = 0; i < N_PHYS_MEM_POOLS; i++) {
        global_dram_mem[i] = &static_global_dram_mem[i];
    }

    /* Step-2: init the buddy allocators for each continuous range of the
     * physmem. */
    for (physmem_map_idx = 0; physmem_map_idx < physmem_map_num;
         ++physmem_map_idx) {
        free_mem_start = physmem_map[physmem_map_idx][0];
        free_mem_end = physmem_map[physmem_map_idx][1];
        /* use global memory pool */
        /* Hybrid DRAM and NVM memory pool */
        init_buddy_for_one_mem_pool(global_dram_mem[physmem_map_idx],
                                    DRAM_PAGE,
                                    free_mem_start,
                                    free_mem_end);
    }

    /* Step-3: init the slab allocator. */
    init_dram_slab();
#endif
}

void mm_init(void *physmem_info, int clear_nvm)
{
    /* Step-0: for system shutdown, we pass info=NULL */
    if (!physmem_info)
        goto skip_parse_info;

    /* Step-1: parse the physmem_info to get each continuous range of the
     * physmem. */
    physmem_map_num = 0;
#ifdef DSM_SHM_DEVICE_CXL_NUMA
    extern void parse_numa_mem_map();
    parse_numa_mem_map();
#else
    parse_mem_map(physmem_info);
#endif
#ifdef USE_NVM
    nvmmem_map_num = 0;
    parse_nvm_map();
#endif
skip_parse_info:
#ifdef DSM_LINEAR_MM_LAYOUT
    /* Use a temporal memory pool to init local structures */
    /* Init dram pool */
    global_temp_mem = &static_global_temp_mem;

    /* Use temp_mem_start and temp_mem_size (1G) for temp allocator */
    extern paddr_t temp_mem_start;
    extern u64 temp_mem_size;
    
    kinfo(ANSI_COLOR_MAGENTA "[TEMP MEMORY] Initializing temp allocator: 0x%lx - 0x%lx (size: 0x%lx)" ANSI_COLOR_RESET "\n",
        temp_mem_start, temp_mem_start + temp_mem_size, temp_mem_size);

    /* use global memory pool */
    /* Hybrid DRAM and NVM memory pool */
    init_buddy_for_one_mem_pool(
            global_temp_mem, TEMP_PAGE, temp_mem_start, temp_mem_start + temp_mem_size);

    init_temp_slab();
    return;
#endif

    // dram_mm_init();

#ifdef USE_NVM /* use NVM as main memory */
#ifdef CHCORE_SLS
    /* The first NVM map is used as metadata */
    nvm_metadata = (struct nvm_metadata *)phys_to_virt(nvmmem_map[0][0]);
#ifndef RESTORE_ENABLED
    nvm_metadata_reset();
#else
    if (clear_nvm)
        nvm_metadata_reset_crash_flag();
    if (!NVM_IS_CRASH)
        nvm_metadata_reset();
#endif /* RESTORE ENABLED */
#endif /* CHCORE_SLS */

    for (physmem_map_idx = 0; physmem_map_idx < nvmmem_map_num;
         ++physmem_map_idx)
        init_buddy_for_one_nvmmem_map(physmem_map_idx);

    /* Step-3: init the slab allocator. */
    init_slab();
#endif /* USE_NVM */
#ifdef CHCORE_SLS
    /* set false */
    nvm_metadata = &nvm_metadata_for_dram;
#endif
}

void ext_mm_init()
{
#ifdef USE_CXL_MEM
    int i = 0, cxlmem_map_idx = 0;
    paddr_t free_mem_start = 0, free_mem_end = 0;

    cxlmem_map_num = 0;
    parse_cxlmem_map();

    /*
     * use shared memory to init mem pool
     * so memory allocator can be shared between machines
     */
    for (i = 0; i < N_PHYS_MEM_POOLS; i++) {
        // global_cxl_mem[i] = &static_global_cxl_mem[i];
        global_cxl_mem[i] = &(dsm_meta->mem_pool[i]);
    }

    /* memory model has been inited */
    if (DSM_STATE >= DSM_CONFIG_STATE_MM_INITED) {
        kinfo(ANSI_COLOR_MAGENTA "[CXL MEMORY] cxl mem pool has been inited" ANSI_COLOR_RESET "\n");
        goto skip_init;
    }

    /* Step-2: init the buddy allocators for each continuous range
     * of the physmem. */
    for (cxlmem_map_idx = 0; cxlmem_map_idx < cxlmem_map_num;
         ++cxlmem_map_idx) {
        free_mem_start = cxlmem_map[cxlmem_map_idx][0];
        free_mem_end = cxlmem_map[cxlmem_map_idx][1];

#ifdef DSM_CXL_LF_BUDDY
        init_buddy_lf(cxlmem_map_idx,
                      global_cxl_mem[cxlmem_map_idx],
                      CXL_MEM_PAGE,
                      free_mem_start,
                      free_mem_end);
#else
        init_buddy_for_one_mem_pool(global_cxl_mem[cxlmem_map_idx],
                                    CXL_MEM_PAGE,
                                    free_mem_start,
                                    free_mem_end);
#endif
    }

    init_cxl_slab();

    /* mark metadata as inited */
    DSM_STATE = DSM_CONFIG_STATE_MM_INITED;

skip_init:
    return;
#endif /* USE_CXL_MEM */
}

unsigned long get_free_mem_size(void)
{
    unsigned long size;
    int i;

    size = get_free_mem_size_from_slab();
    for (i = 0; i < physmem_map_num; ++i)
        size += get_free_mem_size_from_buddy(global_mem[i]);

    return size;
}
