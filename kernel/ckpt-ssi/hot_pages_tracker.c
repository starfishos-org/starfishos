#include <common/macro.h>
#include <common/types.h>
#include <common/vars.h>
#include <mm/kmalloc.h>
#include <mm/buddy.h>
#include <ckpt/ckpt-dsm.h>
#include <mm/vmspace.h>
#include <object/cap_group.h>
#include <sched/context.h>
#include <irq/ipi.h>
#include <ckpt/hot_pages_tracker.h>
#include <ckpt/ckpt.h>
#include <common/kvstore.h>
#include <arch/machine/machine.h>
#include <ckpt/hybird_mem.h>

u64 dyn_hotness = 64;
u64 dyn_access_interval = 64;
void sys_set_dyn_args(u64 hotness, u64 access_interval)
{
    dyn_hotness = hotness;
    dyn_access_interval = access_interval;
}

extern int set_pte_write_flag(pte_t *entry, bool flag);
extern int clear_pte_dirty(pte_t *entry);
extern int query_in_pgtbl(void *pgtbl, vaddr_t va, paddr_t *pa, pte_t **entry);

struct list_head active_list[MIGRATE_CPU_NUM];
struct lock active_list_lock[MIGRATE_CPU_NUM];
u64 active_list_size = 0;

#ifdef PARALLEL_LOOP
struct lock migrate_max_time_lock;
u64 migrate_max_time;
u64 ckpt_max_time;
bool check_and_adjust = false;
#endif

void init_hybrid_structs()
{
    /* Init global structure */
    active_list_size = 0;
    for (int i = 0; i < MIGRATE_CPU_NUM; i++) {
        init_list_head(&(active_list[i]));
        lock_init(&(active_list_lock[i]));
    }
#ifdef DYN_ADJUST
    lock_init(&migrate_max_time_lock);
#endif
}

#ifdef HYBRID_MEM

void add_to_active_list(struct page_track_info *track_info)
{
    u64 rr = atomic_fetch_add_64(&active_list_size, 1) % MIGRATE_CPU_NUM;
#if 0
	/* check that this page does not belong to any list already */
	struct page_track_info *item, *tmp;
	bool exist = false;
	for_each_in_list_safe (item, tmp, list_node, &active_list[track_info->active_list_num]) {
		if (item == track_info)
			exist = true;
	}
	BUG_ON(exist);
#endif
    lock(&(active_list_lock[rr]));
    list_add(&(track_info->list_node), &(active_list[rr]));
    unlock(&(active_list_lock[rr]));
    track_info->active = true;
    track_info->active_list_num = rr;
}

void delete_from_active_list(struct page_track_info *track_info)
{
    u64 rr = track_info->active_list_num;
#if 0
	/* check that this page belong to this list */
	struct page_track_info *item, *tmp;
	bool exist = false;
	for_each_in_list_safe (item, tmp, list_node, &active_list[rr]) {
		if (item == track_info)
			exist = true;
	}
	BUG_ON(!exist);
#endif
    lock(&(active_list_lock[rr]));
    list_del(&(track_info->list_node));
    unlock(&(active_list_lock[rr]));
    atomic_fetch_add_64(&active_list_size, -1);
    track_info->active = false;
    track_info->hotness = 0;
}

bool in_active_list(struct page_track_info *track_info)
{
    if (unlikely(track_info == NULL))
        return false;
    return track_info->active;
}

/*
 * track_access: track_access of @page
 * 1. init track info
 * 2. increase hotness and add to active list if needed
 */
int track_access(struct page *page)
{
#ifdef DETAIL_REPORT
    DECLTMR;
    start();
    track_access_count++;
#endif
    /* not initialized */
    BUG_ON(!page);
    struct page_track_info *info;

    lock(&page->lock);
    if (unlikely(page->track_info == NULL)) {
        /* init track info */
        page->track_info = kzalloc(sizeof(*info), __DEFAULT__);
#ifdef DETAIL_REPORT
        track_access_malloc_time += (plat_get_mono_time() - timer_start);
#endif
    }

    info = page->track_info;
    /* page has just been accessed */
    if (unlikely(info->last_access_ckpt == CKPT_VERSION_NUMBER)) {
        unlock(&page->lock);
        return 0;
    }
    /* info is cleared */
    if (unlikely(info->page == NULL)) {
        info->page = page;
        info->active = false;
        info->last_access_ckpt = CKPT_VERSION_NUMBER;
        info->hotness = 0;
    }

    /* update hotness */
    info->hotness++;

    /* update last_access_ckpt */
    info->last_access_ckpt = CKPT_VERSION_NUMBER;

    LOG("page(%p)->hot=%llu, ->last_access_ckpt=%llu\n",
        page,
        info->hotness,
        info->last_access_ckpt);

    /* and add to active list */
    if (info->hotness >= HOTNESS_THRESHOLD && !info->active) {
        add_to_active_list(info);
        LOG("successfully append page(%p) to active list\n", page);
    }
    unlock(&page->lock);

#ifdef DETAIL_REPORT
    track_access_time += stop();
#endif
    return 0;
}

void destory_track_info(struct page *page)
{
    struct page_track_info *info = page->track_info;
    BUG_ON(!info);
    if (info->active) {
        delete_from_active_list(info);
    }
    if (!info->page) {
        info->page = NULL;
        LOG("[ckpt=%llu] [destory_track_info] page=%p\n",
            CKPT_VERSION_NUMBER,
            page);
    }
}

#if defined(PARALLEL_LOOP) && defined(HYBRID_MEM)
/*
 * adjust_tracker_config():
 *
 * 1. if the ckpt thread need to wait > PARALLEL_WAIT_THRESHOLD
 *    for migrating thread
 *	=> upgrade the tracking method to strick the caching
 * 2. if migrating thread finish all works, but there is still
 *    PARALLEL_WAIT_THRESHOLD for the ckpt thread to finish everything
 *	=> downgrade the tracking method to cache more pages
 */
void adjust_tracker_config()
{
    if (likely(check_and_adjust == 0))
        return;

    LOG("[HYBRID] ckpt_max_time=%llu, migrate_max_time=%llu\n",
        ckpt_max_time,
        migrate_max_time);

    /* checkpoint finished first */
    if (ckpt_max_time < migrate_max_time) {
        if (migrate_max_time - ckpt_max_time > CKPT_WAIT_THRESHOLD) {
            /* if wait time too long; should downgrade dynamic hotness */
#if 0
			if (dyn_hotness < MAX_HOTNESS_THRESHOLD) {
				dyn_hotness = dyn_hotness * 2;
				kinfo("[HYBRID] change hotness to %d\n", dyn_hotness);
#else
            if (dyn_access_interval > STRONGEST_ACCESS_INTERVAL) {
                dyn_access_interval /= 2;
                LOG("[HYBRID] downgrade ACCESS_INTERVAL to %d\n",
                    dyn_access_interval);
#endif
            }
        }
    } else { /* migrate finished first */
        if (ckpt_max_time - migrate_max_time > PARALLEL_WAIT_THRESHOLD) {
            /* else just upgrade the dynamic hotness */
#if 0
			if (dyn_hotness > MIN_HOTNESS_THRESHOLD) {
					dyn_hotness = dyn_hotness / 2;
					kinfo("[HYBRID] change hotness to %d\n", dyn_hotness);
#else
            if (dyn_access_interval < WEAKEST_ACCESS_INTERVAL) {
                dyn_access_interval *= 2;
                LOG("[HYBRID] upgrade ACCESS_INTERVAL to %d\n",
                    dyn_access_interval);
#endif
            }
        }
    }
}
#endif

#endif
