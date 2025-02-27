#include <common/radix.h>
#include "ckpt_page.h"
#include <mm/mm.h>
#include <mm/nvm.h>
#include <common/list.h>

/*
 * when the version number of page_0 and page_1 are equal,
 * we treat page_0 as the new one.
 */
inline struct ckpt_page *get_latest_ckpt_page(struct ckpt_page_pair *page_pair)
{
    return page_pair->pages[0].version_number
                           >= page_pair->pages[1].version_number ?
                   &page_pair->pages[0] :
                   &page_pair->pages[1];
}

inline struct ckpt_page *
get_second_latest_ckpt_page(struct ckpt_page_pair *page_pair)
{
    return page_pair->pages[0].version_number
                           >= page_pair->pages[1].version_number ?
                   &page_pair->pages[1] :
                   &page_pair->pages[0];
}

struct ckpt_page_pair *get_page_pair(struct page *page, u64 index)
{
    struct ckpt_page_pair *page_pair;
    struct ckpt_obj_root *obj_root;
    struct ckpt_object *ckpt_obj;
    struct ckpt_pmobject *ckpt_pmo;

    /* get cached page_pair first */
    page_pair = (struct ckpt_page_pair *)page->page_pair;
    if (page_pair) {
#if 0 /* check the correctness of the page pair */
        obj_root = container_of(page->pmo, struct object, opaque)->obj_root;
        BUG_ON(!obj_root);
        ckpt_obj = obj_root->ckpt_objs[1 - get_current_ckpt_version() % 2];
        BUG_ON(!ckpt_obj);
        ckpt_pmo = (struct ckpt_pmobject*)ckpt_obj->opaque;

        struct ckpt_page_pair *page_pair2 = radix_get(ckpt_pmo->radix, index);
        BUG_ON(page_pair2 != page_pair);
#endif
        return page_pair;
    }

    /* if not found, qeury from the ckpt pmo */
    obj_root = container_of(page->pmo, struct object, opaque)->obj_root;
    BUG_ON(!obj_root);
    ckpt_obj = obj_root->ckpt_objs[1 - get_current_ckpt_version() % 2];
    BUG_ON(!ckpt_obj);
    ckpt_pmo = (struct ckpt_pmobject *)ckpt_obj->opaque;

    page_pair = radix_get(ckpt_pmo->radix, index);

    /* alloc page_pair if not exist */
    /* lock to avoid concurrent*/
    lock(&ckpt_pmo->lock);
    if (unlikely(!page_pair)) {
        page_pair = kzalloc(sizeof(*page_pair), __DEFAULT__);
        page_pair->pages[0].va = (vaddr_t)get_pages(0, __DEFAULT__);
        page_pair->pages[1].va = (vaddr_t)page_to_virt(page);
        radix_add(ckpt_pmo->radix, index, page_pair);
    }
    unlock(&ckpt_pmo->lock);

    /* cache page pair */
    page->page_pair = (u64)page_pair;
    return page_pair;
}

void free_page_pair(struct ckpt_page_pair *page_pair)
{
    if (page_pair->pages[0].va) {
        kfree((void *)page_pair->pages[0].va);
    }
    if (page_pair->pages[1].va) {
        kfree((void *)page_pair->pages[1].va);
    }
    kfree(page_pair);
}

#ifdef REPORT_HYBRID_MEM_DEBUG
extern u64 page_copy_time[PLAT_CPU_NUM];
extern u64 get_page_pair_time[PLAT_CPU_NUM];
#endif

#if 0
/* ckpt_nvm_page: copy nvm page to ckpt_page */
int ckpt_nvm_page(struct pmobject *pmo, void *kva, u64 index) 
{
    u64 current_version = get_current_ckpt_version();
    struct ckpt_obj_root* obj_root = container_of(pmo, struct object, opaque)->obj_root;
    if (!obj_root) {
        return -1;
    }
    struct page *page = virt_to_page(kva);
    struct ckpt_page_pair *page_pair = get_page_pair(page, index);
    struct ckpt_page *ckpt_page = get_latest_ckpt_page(page_pair);

    /* nvm page use COW method, so we should update the latest ckpt page */
    // BUG_ON(ckpt_page->va != page_pair->pages[0].va);
    /* dram page use selective copy or direct-copy method,so we should update the second latest ckpt page */
    // ckpt_page = get_second_latest_ckpt_page(page_pair);

    if (ckpt_page->version_number != current_version) {
        BUG_ON((void*)ckpt_page->va == kva);
        pagecpy_nt((void*)ckpt_page->va, kva);
        ckpt_page->version_number = current_version;
    }

    return 0;
}
#endif
/* ckpt_dram_cached_page: copy dram-cached page to ckpt_page */
void ckpt_dram_cached_page(struct pmobject *pmo, void *kva, u64 index)
{
#ifdef REPORT_HYBRID_MEM_DEBUG
    DECLTMR;
    start();
#endif
    u64 current_version = get_current_ckpt_version();
    struct ckpt_page_pair *page_pair;
    struct page *page;
    struct ckpt_page *ckpt_page;

    page = virt_to_page(kva);
    BUG_ON(get_page_type(page) != DRAM_CACHED_PAGE);

    BUG_ON(page->pmo != pmo);
    page_pair = get_page_pair(page, index);
    BUG_ON(!page_pair);

    /* dram-cached page use selective copy or direct-copy method,
    so we should update the second latest ckpt page */
    ckpt_page = get_second_latest_ckpt_page(page_pair);
#ifdef REPORT_HYBRID_MEM_DEBUG
    u64 get_page_pair = stop();
    get_page_pair_time[smp_get_cpu_id()] += get_page_pair;
#endif
    if (ckpt_page->version_number != current_version) {
        pagecpy_nt((void *)ckpt_page->va, kva);
        ckpt_page->version_number = current_version;
    }
#ifdef REPORT_HYBRID_MEM_DEBUG
    page_copy_time[smp_get_cpu_id()] += (stop() - get_page_pair);
#endif
}
