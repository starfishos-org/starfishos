#include <mm/rmap.h>
#include <common/list.h>
#include <ckpt/hot_pages_tracker.h>
#include <ckpt/hybird_mem.h>
#include <ckpt/ckpt_data.h>
#include <ckpt/ckpt.h>
#include <perf/measure.h>

#include "ckpt_page.h"
#include "ckpt_object_pool.h"

#ifdef PARALLEL_LOOP
extern struct lock migrate_max_time_lock;
extern u64 migrate_max_time;
extern bool check_and_adjust;
#endif

#ifdef REPORT
extern u64 migrate_pages_time[PLAT_CPU_NUM];
#endif

extern int map_page_in_pgtbl(void *pgtbl, vaddr_t va, paddr_t pa,
                             vmr_prop_t flags, pte_t **out_pte);
extern int remap_page_in_pgtbl(pte_t *entry, paddr_t new_pa);

extern int query_in_pgtbl(void *pgtbl, vaddr_t va, paddr_t *pa, pte_t **entry);
extern int clear_pte_dirty(pte_t *entry);
extern int is_pte_dirty(pte_t *entry);
extern int set_pte_write_flag(pte_t *entry, bool flag);

extern void pagecpy(void *dst, const void *src);

// #define CHECK_REVERSE_INFO
// #define CHECK_MIGRATE

#ifdef REPORT_HYBRID_MEM
u64 dirty_pages_array[PLAT_CPU_NUM];
u64 dram_pages_array[PLAT_CPU_NUM];
u64 remove_pages_array[PLAT_CPU_NUM];
u64 newly_added_pages_array[PLAT_CPU_NUM];

#ifdef REPORT_HYBRID_MEM_DEBUG 
u64 get_pte_time[PLAT_CPU_NUM];
u64 page_copy_time[PLAT_CPU_NUM];
u64 get_page_pair_time[PLAT_CPU_NUM];
#endif

inline void report_hybrid_mem_and_clear(void)
{
    u64 dirty_pages = 0, dram_pages = 0, remove_pages = 0, newly_added_pages = 0;
    for (int i = 0; i < PLAT_CPU_NUM; i++) {
        dirty_pages += dirty_pages_array[i];
        dram_pages += dram_pages_array[i];
        remove_pages += remove_pages_array[i];
        newly_added_pages += newly_added_pages_array[i];
#ifdef REPORT_HYBRID_MEM_DEBUG
        printk("CPU[%d]: get_pte=%d, get_page_pair=%d, page_copy=%d\n", i,
           dram_pages_array[i] ? (get_pte_time[i] / dram_pages_array[i]) : 0,
           dirty_pages_array[i] ?
                   (get_page_pair_time[i] / dirty_pages_array[i]) : 0,
           dirty_pages_array[i] ? (page_copy_time[i] / dirty_pages_array[i])
           : 0
           );
#endif
    }
    printk("active list: (dirty/dram, nvm(added), removed, total)= %d/%d %d %d %d\n",
        dirty_pages, dram_pages, newly_added_pages, remove_pages, active_list_size);
    memset(dirty_pages_array, 0, PLAT_CPU_NUM * sizeof(u64));
    memset(dram_pages_array, 0, PLAT_CPU_NUM * sizeof(u64));
    memset(remove_pages_array, 0, PLAT_CPU_NUM * sizeof(u64));
    memset(newly_added_pages_array, 0, PLAT_CPU_NUM * sizeof(u64));
#ifdef REPORT_HYBRID_MEM_DEBUG
    memset(get_pte_time, 0, PLAT_CPU_NUM * sizeof(u64));
    memset(get_page_pair_time, 0, PLAT_CPU_NUM * sizeof(u64));
    memset(page_copy_time, 0, PLAT_CPU_NUM * sizeof(u64));
#endif
}
#endif

static inline void move_runtime_nvm_page_to_backup(struct pmobject *pmo, void *kva, u64 index)
{
    struct ckpt_obj_root *obj_root;
    struct ckpt_object *ckpt_obj;
    struct ckpt_pmobject *ckpt_pmo;
    struct ckpt_page_pair *page_pair;
    struct ckpt_page *ckpt_page;
    u64 current_version = get_current_ckpt_version();

    obj_root = container_of(pmo, struct object, opaque)->obj_root;
    BUG_ON(!obj_root);
    ckpt_obj = get_latest_ckpt_obj(obj_root, current_version);
    BUG_ON(!ckpt_obj);
    ckpt_pmo = (struct ckpt_pmobject*)ckpt_obj->opaque;
    page_pair = radix_get(ckpt_pmo->radix, index);
    if (!page_pair)
        BUG("page(%p) pmo->type=%d\n", virt_to_page(kva), pmo->type);

    ckpt_page = get_second_latest_ckpt_page(page_pair);
    BUG_ON(ckpt_page->va != (vaddr_t)kva);
    // ckpt_page->va = (vaddr_t)kva;
    ckpt_page->version_number = current_version;
}

static inline void*
move_runtime_nvm_page_from_backup(struct pmobject *pmo, u64 index)
{
    vaddr_t new_va = 0;
    struct ckpt_obj_root *obj_root;
    struct ckpt_object *ckpt_obj;
    struct ckpt_pmobject *ckpt_pmo;
    struct ckpt_page_pair *page_pair;
    struct ckpt_page *ckpt_page, *old_ckpt_page;
    u64 current_version = get_current_ckpt_version();

    obj_root = container_of(pmo, struct object, opaque)->obj_root;
    BUG_ON(!obj_root);
    ckpt_obj = get_latest_ckpt_obj(obj_root, current_version);
    BUG_ON(!ckpt_obj);
    ckpt_pmo = (struct ckpt_pmobject*)ckpt_obj->opaque;
    page_pair = radix_get(ckpt_pmo->radix, index);
    BUG_ON(!page_pair);

    ckpt_page = get_latest_ckpt_page(page_pair);
    old_ckpt_page = get_second_latest_ckpt_page(page_pair);

    /* current accessed page will not be migrated */
    BUG_ON(current_version == old_ckpt_page->version_number
           || current_version == ckpt_page->version_number);

    /* dup ckpt_page to old_ckpt_page */
    pagecpy((void *)old_ckpt_page->va, (void *)ckpt_page->va);
    old_ckpt_page->version_number = ckpt_page->version_number;

    /* when init, we use ckpt_pair[1] as origin page */
    new_va = page_pair->pages[1].va;
    page_pair->pages[1].version_number = 0;

    if (use_continuous_pages(pmo)) {
        BUG_ON(new_va != phys_to_virt(pmo->start + index * PAGE_SIZE));
    }

    return (void *)new_va;
}

static int migrate_page(struct page *old_page, bool to_dram)
{
        struct page *head, *new_page;
        struct reverse_node *item;
        u64 index;
        void *new_va = NULL, *new_pa = NULL, *old_va = NULL;
        struct vmregion *vmr;
        struct pmobject *pmo;
        int ret;
        pte_t *pte;
#ifdef CHECK_MIGRATE
        int reverse = 0;
#endif
        /* get head of the compound pages group this page belongs to */
        head = compound_head(old_page);
        pmo = head->pmo;
        BUG_ON(!pmo);
        index = head->index + compound_head_offset(old_page, head);

        old_va = page_to_virt(old_page);

        if (to_dram) {
            /* allocate new page */
            new_va = get_dram_pages(0);
            /* move runtime nvm page to server as backup */
            move_runtime_nvm_page_to_backup(pmo, old_va, index);
            /* memcpy page (page_va) to new page (new_va) */   
            pagecpy_nt(new_va, old_va);
        } else {
            /* select nvm page from original nvm location */
            new_va = move_runtime_nvm_page_from_backup(pmo, index);
            pagecpy_nt(new_va, old_va);
#ifdef CHECK_MIGRATE
            /* data of new_va should be exactly as current data */
            BUG_ON(strncmp((char *)new_va, (char *)old_va, 4096));
#endif
        }
        BUG_ON(new_va == NULL);
        
        new_pa = (void *)virt_to_phys(new_va);
        new_page = virt_to_page(new_va);

        /* update track info of new_page and old_page */
        new_page->ref_cnt = old_page->ref_cnt;
        new_page->track_info = old_page->track_info;
        new_page->track_info->page = new_page; /* now list will access new_page */
        /* for nvm old_page, we do not clear the track info */

        /* update PG_cached flag */
        if (to_dram)
            page_set_flag(new_page, PG_cached);

        /* 1. update va->pa in vmregion->pmo->radix */    
        if (to_dram) {          
            commit_dram_cached_page(pmo, index, (paddr_t)new_pa);
        } else {
            if (use_radix(pmo)) {
                commit_page_to_pmo(pmo, index, (paddr_t)new_pa);
            } else {
                clear_dram_cached_page(pmo, index);
            }
            /* free old dram page */
            /* track_info should be clear to avoid being destroyed */
            old_page->track_info = NULL;
            free_pages(old_va);
        }

        LOG("[CKPT=%d] to_dram?%d, migrate page: %p (va=0x%llx,flag=%d) => "
                "page: %p (va=0x%llx,flag=%d) \n(pmo(%p)->type=%d)\n",
                get_current_ckpt_version(), to_dram, 
                old_page, old_va, old_page->flags,
                new_page, new_va, new_page->flags,
                pmo, pmo->type);

        /* 2. update pte to new_pa */
        for_each_in_list(item, struct reverse_node, node, &(pmo->reverse_list)) {
            vmr = item->vmr;
            if ((vmr == NULL))
                continue;
            BUG_ON(vmr->pmo != pmo);
#ifdef CHECK_MIGRATE
            reverse += 1;
#endif
            ret = query_in_pgtbl(((struct vmspace *)vmr->vmspace)->pgtbl,
                            vmr->start + index * PAGE_SIZE,
                            NULL,
                            &pte);
            if (ret) {
                // not find this page in pgtbl
                BUG_ON(pmo->type != PMO_SHM);
                LOG("Not mapped in pgtbl\n");
                map_page_in_pgtbl(((struct vmspace *)vmr->vmspace)->pgtbl,
                                  vmr->start + index * PAGE_SIZE,
                                  (paddr_t)new_pa,
                                  vmr->perm,
                                  &pte);
            } else {
                remap_page_in_pgtbl(pte, (paddr_t)new_pa);
            }

            /* mark DRAM_CACHED page as writable if to_dram; 
             * else mark NVM page as non writable */
            set_pte_write_flag(pte, to_dram);
            LOG("hybrid: set page(%p) pte(%p) %d\n", new_page, pte, to_dram);
        }
#ifdef CHECK_MIGRATE
        if (pmo->type == PMO_SHM)
            BUG_ON(reverse <= 1);
        else BUG_ON(reverse != 1);
#endif
        return 0;
}

#define migrate_page_to_dram(x) migrate_page(x, true)
#define migrate_page_to_nvm(x) migrate_page(x, false)

/*
* handle_dram_cached_page - ckpt dram-cached page if it is dirty
*/
static int handle_dram_cached_page(struct page *page)
{
    struct page *head;
    struct reverse_node *item;
    struct pmobject *pmo;
    pte_t *pte;
    u64 index;
    bool dirty = false, _dirty;
#ifdef REPORT_HYBRID_MEM_DEBUG
    DECLTMR;start();
#endif
    /* get head of the compound pages group this page belongs to */
    head = compound_head(page);
    pmo = head->pmo;
    BUG_ON(!pmo);
    index = head->index + compound_head_offset(page, head);

    for_each_in_list(item, struct reverse_node, node, &(pmo->reverse_list)) {
        /* whelther the page is dirty */
        query_in_pgtbl(((struct vmspace *)item->vmr->vmspace)->pgtbl,
                        item->vmr->start + index * PAGE_SIZE,
                        NULL,
                        &pte);
        _dirty = is_pte_dirty(pte);
        dirty |= _dirty;
        if (_dirty)
            clear_pte_dirty(pte);
    }
#ifdef REPORT_HYBRID_MEM_DEBUG
    get_pte_time[smp_get_cpu_id()] += stop();
#endif

    if (dirty) {
        /* 1. ckpt this page */
        ckpt_dram_cached_page(pmo, page_to_virt(page), index);
        
        /* 2. update track info */
        page->track_info->last_access_ckpt = CKPT_VERSION_NUMBER;
        page->track_info->hotness++;
#ifdef REPORT_HYBRID_MEM
        dirty_pages_array[smp_get_cpu_id()]++;
#endif
    }

    return 0;
}

/* whelther a page is migrateable */
static inline bool migrateable_page(struct page *page) {
    pmo_type_t type = page->pmo->type;
    if (type == PMO_DEVICE || type == PMO_RING_BUFFER
        || type == PMO_RING_BUFFER_RADIX)
        return false;
    else
        return true;
}

/* pages not accessed ACCESS_INTERVAL times should be removed */
static bool need_remove_from_active_list(struct page *page)
{
    u64 last_access_ckpt = page->track_info->last_access_ckpt;
    return CKPT_VERSION_NUMBER - last_access_ckpt > ACCESS_INTERVAL;
}

/*
 * process_sub_active_list - migrate pages between DRAM and NVM devices
 * 
 * DRAM - K hotest pages
 * NVM - other pages
 * 
 * migrate_pages is called during ckpt to aviod conflicts with
 * running theads who acess these migrating pages.
 * 
 * two part:
 * 1. move new hot pages from NVM to DRAM
 * 2. move pages which is removed from active list during last
 * ckpt from DRAM to NVM
 */
void process_sub_active_list(struct list_head *sublist)
{
    struct page *page;
    struct page_track_info *item, *tmp;
    page_type_t page_type;

#ifdef REPORT
    DECLTMR;start();
#endif
#ifdef DYN_ADJUST
    DECLTMR2;
    if (unlikely(check_and_adjust)) {
        start2();
    }
#endif
    /* called during ckpt, we assume that lock is only acquired by this function */
    for_each_in_list_safe(item, tmp, list_node, sublist) {
        /* for the item in sublist */
        page = item->page;
        BUG_ON(page == NULL);
        page_type = get_page_type(page);
        BUG_ON(page_type != DRAM_CACHED_PAGE && page_type != NVM_PAGE);

        switch (page_type) {
            case DRAM_CACHED_PAGE:
                /* ckpt dram cached pages */
                BUG_ON(handle_dram_cached_page(page));
                /* check whelther this page should be moved from active list */
                if (need_remove_from_active_list(page)) {
                    /* 1. migrated to NVM page */
                    BUG_ON(migrate_page_to_nvm(page));
                    /* 2. remove from active list */
                    delete_from_active_list(item);
            #ifdef REPORT_HYBRID_MEM
                remove_pages_array[smp_get_cpu_id()]++;
            #endif  
                }     
            #ifdef REPORT_HYBRID_MEM
                dram_pages_array[smp_get_cpu_id()]++;
            #endif     
                break;
            case NVM_PAGE:
                /* page is still on NVM */
                BUG_ON(migrate_page_to_dram(page));
            #ifdef REPORT_HYBRID_MEM
                newly_added_pages_array[smp_get_cpu_id()]++;
            #endif
                break;
            default:
                BUG("%s: invalid page in active list.\n", __func__);
                break;
        }
    }

#ifdef DYN_ADJUST
    /* atomic setting migrate_max_time */
    if (unlikely(check_and_adjust)) {
        lock(&migrate_max_time_lock);
        u64 migrate_time = stop2();
        if (migrate_time > migrate_max_time)
            migrate_max_time = migrate_time;
        unlock(&migrate_max_time_lock);
    }
#endif
#ifdef REPORT
    migrate_pages_time[smp_get_cpu_id()] = stop();
#endif
}

void prepare_process_active_list(void)
{
/* ckpt CPU side */
#ifdef PARALLEL_LOOP
    /* signal parellal loop */
    extern void signal_parallel_active_list_loop();
    signal_parallel_active_list_loop();
#else
    for (int i = 0; i < MIGRATE_CPU_NUM; i++) 
        process_sub_active_list(&active_list[i]);
#endif
    // LOG("prepare_process_active_list finish: total %lu objects\n", active_list_size);
}

void finish_process_active_list(void)
{
#ifdef PARALLEL_LOOP
    extern void wait_parallel_active_list_loop();
    wait_parallel_active_list_loop();
#endif
}
