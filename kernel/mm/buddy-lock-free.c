/*
 * CXL allocator backend switched to llfree.
 * Keep legacy buddy_lf_* symbols so existing call sites do not change.
 */

#ifdef USE_CXL_MEM

#include <common/kprint.h>
#include <common/macro.h>
#include <common/types.h>
#include <common/util.h>
#include <arch/mmu.h>
#include <arch/machine/smp.h>
#include <mm/buddy.h>
#include <mm/kmalloc.h>
#include <mm/llfree/llfree.h>
#include <mm/mm.h>

extern struct phys_mem_pool *global_cxl_mem[];
extern int cxlmem_map_num;

#if defined(CHCORE_SLS) && defined(HYBRID_MEM)
extern void destory_track_info(struct page *page);
#endif

struct buddy_llfree_layout {
	size_t frames;
	llfree_meta_size_t meta_sz;
	paddr_t llfree_phys;
	paddr_t local_phys;
	paddr_t trees_phys;
	paddr_t lower_phys;
	paddr_t page_meta_phys;
	paddr_t data_phys;
};

struct buddy_lf_ctx {
	struct phys_mem_pool *pool;
	llfree_t *llfree;
	size_t frames;
	size_t locals;
};

static struct buddy_lf_ctx buddy_lf_cxl[N_PHYS_MEM_POOLS];

#define LLFREE_MIN_PAGES ((size_t)LLFREE_TREE_SIZE)
#define LLFREE_MAX_PAGES ((size_t)1 << 28)

static inline bool add_overflow_u64(u64 a, u64 b, u64 *out)
{
	*out = a + b;
	return *out < a;
}

static inline u64 align_up_u64(u64 x, u64 a)
{
	return (x + a - 1ULL) & ~(a - 1ULL);
}

static bool compute_llfree_layout(paddr_t free_mem_start, paddr_t free_mem_end,
				  size_t locals,
				  struct buddy_llfree_layout *out)
{
	llfree_tiering_t tiering;
	u64 llfree_phys;
	u64 avail_pages;
	size_t frames;
	int iter;

	if (free_mem_end <= free_mem_start)
		return false;

	llfree_phys = align_up_u64((u64)free_mem_start, (u64)LLFREE_CACHE_SIZE);
	if (llfree_phys >= free_mem_end)
		return false;

	avail_pages = (free_mem_end - llfree_phys) / BUDDY_PAGE_SIZE;
	if (avail_pages < LLFREE_MIN_PAGES)
		return false;

	frames = (size_t)avail_pages;
	if (frames > LLFREE_MAX_PAGES)
		frames = LLFREE_MAX_PAGES;

	tiering = llfree_tiering_simple(locals);

	for (iter = 0; iter < 64; ++iter) {
		llfree_meta_size_t meta_sz;
		u64 llfree_bytes;
		u64 local_phys;
		u64 trees_phys;
		u64 lower_phys;
		u64 after_lower;
		u64 page_meta_phys;
		u64 page_meta_end;
		u64 data_phys;
		size_t fit_frames;

		meta_sz = llfree_metadata_size(&tiering, frames);
		llfree_bytes = align_up_u64((u64)meta_sz.llfree, (u64)LLFREE_CACHE_SIZE);

		if (add_overflow_u64(llfree_phys, llfree_bytes, &local_phys))
			return false;
		if (add_overflow_u64(local_phys, meta_sz.local, &trees_phys))
			return false;
		if (add_overflow_u64(trees_phys, meta_sz.trees, &lower_phys))
			return false;
		if (add_overflow_u64(lower_phys, meta_sz.lower, &after_lower))
			return false;

		page_meta_phys = align_up_u64(after_lower, 16ULL);
		if (add_overflow_u64(page_meta_phys,
				     (u64)frames * sizeof(struct page),
				     &page_meta_end))
			return false;
		data_phys = align_up_u64(page_meta_end, (u64)BUDDY_PAGE_SIZE);

		if (data_phys >= free_mem_end)
			fit_frames = 0;
		else
			fit_frames = (size_t)((free_mem_end - data_phys) / BUDDY_PAGE_SIZE);

		if (fit_frames > LLFREE_MAX_PAGES)
			fit_frames = LLFREE_MAX_PAGES;
		if (fit_frames < LLFREE_MIN_PAGES)
			return false;

		if (fit_frames == frames) {
			out->frames = frames;
			out->meta_sz = meta_sz;
			out->llfree_phys = (paddr_t)llfree_phys;
			out->local_phys = (paddr_t)local_phys;
			out->trees_phys = (paddr_t)trees_phys;
			out->lower_phys = (paddr_t)lower_phys;
			out->page_meta_phys = (paddr_t)page_meta_phys;
			out->data_phys = (paddr_t)data_phys;
			return true;
		}

		frames = fit_frames;
	}

	return false;
}

static inline struct buddy_lf_ctx *lf_ctx_for_pool(struct phys_mem_pool *pool)
{
	int i;

	for (i = 0; i < cxlmem_map_num; ++i) {
		if (buddy_lf_cxl[i].pool == pool)
			return &buddy_lf_cxl[i];
	}
	return NULL;
}

static void init_page_meta_shared(struct phys_mem_pool *pool, size_t frames)
{
	size_t i;

	memset(pool->page_metadata, 0, frames * sizeof(struct page));
	for (i = 0; i < frames; ++i) {
		struct page *p = pool->page_metadata + i;
		p->pool = pool;
		p->order = 0;
		p->flags = 0;
		p->slab = NULL;
		p->ref_cnt = 0;
#ifdef RMAP_ENABLED
		p->pmo = NULL;
		p->index = 0;
		clear_compound_head(p);
#endif
#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
		p->page_pair = 0;
#endif
	}
}

static void attach_page_meta_shared(struct phys_mem_pool *pool, size_t frames)
{
	size_t i;

	for (i = 0; i < frames; ++i)
		(pool->page_metadata + i)->pool = pool;
}

static void init_buddy_lf_common(int cxl_pool_idx, struct phys_mem_pool *pool,
				 page_type_t type, paddr_t free_mem_start,
				 paddr_t free_mem_end, uint8_t llfree_init_mode)
{
	struct buddy_llfree_layout layout;
	llfree_tiering_t tiering;
	llfree_meta_t meta;
	llfree_result_t res;
	size_t locals = (size_t)PLAT_CPU_NUM;
	int i;

	BUG_ON(type != CXL_MEM_PAGE);
	BUG_ON(cxl_pool_idx < 0 || cxl_pool_idx >= N_PHYS_MEM_POOLS);
	BUG_ON(cxl_pool_idx >= cxlmem_map_num);

	if (locals == 0)
		locals = 1;

	if (!compute_llfree_layout(free_mem_start, free_mem_end, locals, &layout))
		BUG("llfree: unable to fit metadata and pages in CXL range\n");
	BUG_ON(layout.llfree_phys < free_mem_start);
	BUG_ON(layout.data_phys < free_mem_start);
	BUG_ON(layout.data_phys >= free_mem_end);

	if (llfree_init_mode == LLFREE_INIT_FREE)
		BUG_ON(lock_init(&pool->buddy_lock) != 0);

	pool->pool_start_addr = (vaddr_t)phys_to_virt(layout.data_phys);
	pool->page_metadata = (struct page *)(void *)phys_to_virt(layout.page_meta_phys);
	pool->pool_mem_size = layout.frames * BUDDY_PAGE_SIZE;
	pool->pool_phys_page_num = layout.frames;
	pool->type = type;

	for (i = 0; i < BUDDY_MAX_ORDER; ++i) {
		pool->free_lists[i].nr_free = 0;
		init_list_head(&pool->free_lists[i].free_list);
	}

	if (llfree_init_mode == LLFREE_INIT_FREE)
		init_page_meta_shared(pool, layout.frames);
	else
		attach_page_meta_shared(pool, layout.frames);

	tiering = llfree_tiering_simple(locals);
	meta = (llfree_meta_t){
		.local = (uint8_t *)(void *)phys_to_virt(layout.local_phys),
		.trees = (uint8_t *)(void *)phys_to_virt(layout.trees_phys),
		.lower = (uint8_t *)(void *)phys_to_virt(layout.lower_phys),
	};

	res = llfree_init((llfree_t *)(void *)phys_to_virt(layout.llfree_phys),
			 layout.frames, llfree_init_mode, meta, &tiering);
	BUG_ON(!llfree_is_ok(res));

	buddy_lf_cxl[cxl_pool_idx].pool = pool;
	buddy_lf_cxl[cxl_pool_idx].llfree =
		(llfree_t *)(void *)phys_to_virt(layout.llfree_phys);
	buddy_lf_cxl[cxl_pool_idx].frames = layout.frames;
	buddy_lf_cxl[cxl_pool_idx].locals = locals;

	kinfo("[LLFREE_%s] pool %p type %d frames %lu locals %lu data @ 0x%lx meta @ 0x%lx\n",
	      llfree_init_mode == LLFREE_INIT_FREE ? "INIT" : "ATTACH", pool,
	      type, (unsigned long)layout.frames, (unsigned long)locals,
	      (unsigned long)layout.data_phys,
	      (unsigned long)layout.llfree_phys);
}

void init_buddy_lf(int cxl_pool_idx, struct phys_mem_pool *pool,
		   page_type_t type, paddr_t free_mem_start,
		   paddr_t free_mem_end)
{
	init_buddy_lf_common(cxl_pool_idx, pool, type, free_mem_start,
			     free_mem_end, LLFREE_INIT_FREE);
}

void attach_buddy_lf(int cxl_pool_idx, struct phys_mem_pool *pool,
		     page_type_t type, paddr_t free_mem_start,
		     paddr_t free_mem_end)
{
	init_buddy_lf_common(cxl_pool_idx, pool, type, free_mem_start,
			     free_mem_end, LLFREE_INIT_NONE);
}

struct page *buddy_lf_get_pages(struct phys_mem_pool *pool, int order)
{
	struct buddy_lf_ctx *ctx = lf_ctx_for_pool(pool);
	llfree_result_t res;
	llfree_request_t req;
	unsigned long page_idx;
	struct page *page;
	int i;
	unsigned long total_pages;

	if (unlikely(order < 0 || order >= BUDDY_MAX_ORDER)) {
		kwarn("llfree: order too large\n");
		return NULL;
	}
	if (unlikely((unsigned)order > LLFREE_MAX_ORDER)) {
		kwarn("llfree: unsupported order=%d (max=%u)\n", order,
		      LLFREE_MAX_ORDER);
		return NULL;
	}
	if (!ctx)
		return NULL;

	req = llfree_simple_request(ctx->locals, (uint8_t)order,
				   (size_t)smp_get_cpu_id());
	res = llfree_get(ctx->llfree, ll_none(), req);
	if (!llfree_is_ok(res))
		return NULL;
	page_idx = (unsigned long)res.frame;

	total_pages = 1UL << order;
	if (unlikely(page_idx + total_pages > ctx->frames))
		BUG("llfree returned out-of-range frame=%lu order=%d frames=%lu\n",
		    page_idx, order, (unsigned long)ctx->frames);

	page = pool->page_metadata + page_idx;
	for (i = 0; i < (int)total_pages; ++i) {
		struct page *p = page + i;
		if (unlikely(page_check_flag(p, PG_allocated))) {
			kwarn("llfree: duplicate alloc page_idx=%lu order=%d cpu=%u pool=%p\n",
			      page_idx + (unsigned long)i, order,
			      (unsigned)smp_get_cpu_id(), pool);
			BUG_ON(1);
		}
#ifdef RMAP_ENABLED
		set_compound_head(p, page);
#endif
		page_set_flag(p, PG_allocated);
		p->pool = pool;
	}

	page->order = order;
	return page;
}

void buddy_lf_free_pages(struct phys_mem_pool *pool, struct page *page)
{
	struct buddy_lf_ctx *ctx = lf_ctx_for_pool(pool);
	unsigned long idx;
	int ord = page->order;
	int i;
	llfree_request_t req;
	llfree_result_t res;
	unsigned long total_pages;

	if (!ctx)
		BUG("buddy_lf_free_pages: no ctx\n");
	if (unlikely(ord < 0 || (unsigned)ord > LLFREE_MAX_ORDER))
		BUG("buddy_lf_free_pages: unsupported order=%d\n", ord);

#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
	prepare_latest_log(pool, REMOVE_PAGES, (u64)page, page->order, 0);
#endif

	idx = (unsigned long)(page - pool->page_metadata);
	total_pages = 1UL << ord;
	if (unlikely(idx + total_pages > ctx->frames))
		BUG("buddy_lf_free_pages: out-of-range idx=%lu order=%d frames=%lu\n",
		    idx, ord, (unsigned long)ctx->frames);

	for (i = 0; i < (int)total_pages; ++i) {
		struct page *p = page + i;

		BUG_ON(!page_check_flag(p, PG_allocated));
		p->flags = 0;
#ifdef RMAP_ENABLED
		clear_compound_head(p);
		p->pmo = NULL;
		p->index = 0;
#endif
#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
		p->page_pair = 0;
#endif
	}
#if defined(CHCORE_SLS) && defined(HYBRID_MEM)
	if (page->track_info)
		destory_track_info(page);
#endif

	req = llfree_simple_request(ctx->locals, (uint8_t)ord,
				   (size_t)smp_get_cpu_id());
	res = llfree_put(ctx->llfree, idx, req);
	BUG_ON(!llfree_is_ok(res));

#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
	commit_latest_log(pool);
#endif
}

#endif /* USE_CXL_MEM */
