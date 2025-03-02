#pragma once

#include <common/types.h>
#include <mm/vmspace.h>
#include <common/util.h>
#include <arch/mm/page_table.h>
#include <mm/nvm.h>

// #define LOG(fmt, ...) kinfo(fmt, ##__VA_ARGS__);
#define LOG(fmt, ...)

/*
 * Configurations for tracking and migrating pages
 *
 * HOTNESS_THRESHOLD:
 * 	migrate page with hotness > HOTNESS_THRESHOLD
 * ACCESS_INTERVAL:
 * 	if a page is not accesed any more for `ACCESS_INTERVAL` ckpt,
 *   remove from active list and migrate from DRAM to NVM
 */
#if 0 /* Old static config */
#define ACCESS_INTERVAL   50
#define HOTNESS_THRESHOLD 50
#else /* New dynamic config with dynamic HOTNESS */
#define MIN_HOTNESS_THRESHOLD (16)
#define MAX_HOTNESS_THRESHOLD (4096)
extern u64 dyn_hotness;
#define HOTNESS_THRESHOLD (dyn_hotness)

#define STRONGEST_ACCESS_INTERVAL (16)
#define WEAKEST_ACCESS_INTERVAL   (4096)
extern u64 dyn_access_interval;
#define ACCESS_INTERVAL (dyn_access_interval)
#endif

/*
 * CKPT_WAIT_THRESHOLD:
 * 1. if the ckpt thread need to wait > 10us for migrating thread
 *		=> upgrade the tracking method to strick the caching
 * PARALLEL_WAIT_THRESHOLD:
 * 2. if migrating thread finish all works, but there is still 10us
 * 	 for the ckpt thread to finish everything
 *		=> downgrade the tracking method to cache more pages
 */
#define PARALLEL_WAIT_THRESHOLD (50 * 1000) /* 50us */
#define CKPT_WAIT_THRESHOLD     (10 * 1000) /* 10us */

/*
 * CHECK_FREQ:
 * 	the adjusting of tracking & migrating method is taken every CHECK_FREQ
 * ckpt
 */
#define CHECK_FREQ (100)

/*
 * page patch entry and pool
 */
#define MAX_ENTRY 250

struct pte_patch_pool_entry {
    pte_t *pte;
    struct page *page;
};

struct pte_patch_pool {
    u64 count;
    struct pte_patch_pool *next;
    struct pte_patch_pool_entry array[MAX_ENTRY]; //  16 byte * 256 = 4K
} __attribute__((aligned(PAGE_SIZE)));

/*
 * page hotness tracker
 */
struct virt_info {
    pte_t *pte;
    vaddr_t va;
    struct vmregion *vmr;
    struct vmspace *vmspace;
    struct virt_info *next;
};

struct page_track_info {
    struct list_head list_node;
    struct page *page;
    bool active;
    u32 active_list_num;
    u64 last_access_ckpt;
    u64 hotness;
};

void add_pte_patch_to_pool(struct vmspace *vmspace, pte_t *pte,
                           struct page *page);

void add_to_active_list(struct page_track_info *);
void delete_from_active_list(struct page_track_info *);
bool in_active_list(struct page_track_info *);

/*
 * init_page_track_info: init track info
 */
int init_page_track_info(struct page *page);

/*
 * track_access: track access of @va at @ckpt_id
 */
int track_access(struct page *page);

void destory_track_info(struct page *page);

void init_hybrid_structs();

#if defined(PARALLEL_LOOP) && defined(HYBRID_MEM)
/*
 * dynamically adjust tracker config
 * downgrade or upgrade
 */
void adjust_tracker_config();
#endif
