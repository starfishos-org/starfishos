#include "llfree_inner.h"

#include <common/util.h> /* memset */

#define RETRIES 8

#define LLFREE_TRACE_GET 0

#if LLFREE_TRACE_GET
typedef struct llfree_get_trace {
	bool enabled;
	uint64_t req_id;
	uint32_t local_attempts;
	uint32_t local_misses;
	uint32_t local_retries;
	uint32_t reserve_attempts;
	uint32_t reserve_misses;
} llfree_get_trace_t;

static uint64_t llfree_get_trace_seq;

static inline uint64_t ll_get_trace_next_id(void)
{
	return (uint64_t)atomic_fetch_add_64((s64 *)&llfree_get_trace_seq, 1);
}

#define LLGET_TRACE(trace, fmt, ...)                                                     \
	do {                                                                             \
		if ((trace) != NULL && (trace)->enabled)                                \
			llfree_info("[get#%llu] " fmt,                                  \
				    (unsigned long long)(trace)->req_id, ##__VA_ARGS__); \
	} while (0)
#else
typedef struct llfree_get_trace {
	bool enabled;
	uint64_t req_id;
	uint32_t local_attempts;
	uint32_t local_misses;
	uint32_t local_retries;
	uint32_t reserve_attempts;
	uint32_t reserve_misses;
} llfree_get_trace_t;

#define LLGET_TRACE(trace, fmt, ...) \
	do {                         \
		(void)(trace);       \
	} while (0)
#endif

static treeF_t init_tree_cb(size_t tree_start_frame, void *ctx)
{
	lower_t *lower = (lower_t *)ctx;
	size_t tree_idx = tree_start_frame / LLFREE_TREE_SIZE;
	treeF_t sum = 0;
	for (size_t child_idx = 0; child_idx < LLFREE_TREE_CHILDREN; ++child_idx) {
		child_t child = atom_load(&lower->children[tree_idx].entries[child_idx]);
		sum += (treeF_t)child.free;
	}
	return sum;
}

llfree_meta_size_t llfree_metadata_size(const llfree_tiering_t *tiering, size_t frames)
{
	llfree_meta_size_t meta = {
		.llfree = sizeof(llfree_t),
		.trees = trees_metadata_size(frames),
		.local = ll_local_size(tiering),
		.lower = lower_metadata_size(frames),
	};
	return meta;
}

static bool check_meta(llfree_meta_t meta, llfree_meta_size_t sizes)
{
	if ((size_t)meta.local % LLFREE_CACHE_SIZE != 0 || (size_t)meta.trees % LLFREE_CACHE_SIZE != 0 ||
	    (size_t)meta.lower % LLFREE_CACHE_SIZE != 0)
		return false;

	return (meta.local + sizes.local <= meta.lower || meta.lower + sizes.lower <= meta.local) &&
	       (meta.local + sizes.local <= meta.trees || meta.trees + sizes.trees <= meta.local) &&
	       (meta.lower + sizes.lower <= meta.trees || meta.trees + sizes.trees <= meta.lower);
}

llfree_result_t llfree_init(llfree_t *self, size_t frames, uint8_t init, llfree_meta_t meta,
			    const llfree_tiering_t *tiering)
{
	llfree_assert(self != NULL);
	llfree_assert(tiering != NULL);
	llfree_assert(tiering->num_tiers >= 1 && tiering->num_tiers <= LLFREE_MAX_TIERS);
	llfree_assert(check_meta(meta, llfree_metadata_size(tiering, frames)));

	if (init >= LLFREE_INIT_MAX)
		return llfree_err(LLFREE_ERR_INIT);
	if (frames < MIN_PAGES || frames > MAX_PAGES)
		return llfree_err(LLFREE_ERR_INIT);

	llfree_result_t res = lower_init(&self->lower, frames, init, meta.lower);
	if (!llfree_is_ok(res))
		return res;

	trees_init_fn init_fn = (init != LLFREE_INIT_NONE) ? init_tree_cb : NULL;
	trees_init(&self->trees, frames, meta.trees, init_fn, &self->lower, tiering->default_tier);

	self->local = (local_t *)meta.local;
	ll_local_init(self->local, tiering);

	self->policy = tiering->policy;
	self->num_tiers = (uint8_t)tiering->num_tiers;

	return llfree_ok(0, 0);
}

static void swap_reserved(llfree_t *self, uint8_t tier, size_t index, size_t new_idx,
			  treeF_t new_free)
{
	local_result_t old = ll_local_swap(self->local, tier, index, new_idx, new_free);
	llfree_assert(old.success);
	if (old.present) {
		trees_unreserve(&self->trees, tree_from_row(old.start_row), old.free, old.tier,
				self->policy);
	}
}

struct check_args {
	uint8_t tier;
	p_range_t free;
	llfree_policy_fn policy;
};

static uint8_t check_reserve_tree(uint8_t tree_tier, treeF_t frames, void *args)
{
	struct check_args *a = (struct check_args *)args;
	if (frames < a->free.min || frames > a->free.max)
		return LLFREE_TIER_NONE;
	llfree_policy_t p = a->policy(a->tier, tree_tier, frames);
	if (p.type == LLFREE_POLICY_MATCH)
		return a->tier;
	if (p.type == LLFREE_POLICY_DEMOTE && frames == LLFREE_TREE_SIZE)
		return a->tier;
	return LLFREE_TIER_NONE;
}

typedef struct reserve_args {
	llfree_t *self;
	uint8_t order;
	uint8_t tier;
	size_t local;
	void *check_args;
	llfree_get_trace_t *trace;
} reserve_args_t;

static llfree_result_t get_reserve(size_t idx, void *ctx)
{
	reserve_args_t *rargs = (reserve_args_t *)ctx;
	llfree_t *self = rargs->self;

	if (rargs->trace != NULL && rargs->trace->enabled)
		rargs->trace->reserve_attempts++;

	treeF_t old_free;
	uint8_t target_tier;
	if (!trees_reserve(&self->trees, idx, check_reserve_tree, rargs->check_args, &old_free,
				   &target_tier)) {
		if (rargs->trace != NULL && rargs->trace->enabled)
			rargs->trace->reserve_misses++;
		return llfree_err(LLFREE_ERR_MEMORY);
	}

	size_t tier_len = ll_local_tier_locals(self->local, target_tier);
	if (tier_len == 0 || tier_len == LLFREE_LOCAL_NONE) {
		if (rargs->trace != NULL && rargs->trace->enabled)
			rargs->trace->reserve_misses++;
		trees_unreserve(&self->trees, idx, old_free, target_tier, self->policy);
		return llfree_err(LLFREE_ERR_MEMORY);
	}
	size_t local = rargs->local % tier_len;

	llfree_result_t res =
		lower_get(&self->lower, frame_from_tree(idx), rargs->order, ll_none());

	if (llfree_is_ok(res)) {
		treeF_t new_free = old_free - (treeF_t)(1u << rargs->order);
		swap_reserved(self, target_tier, local, idx, new_free);
		res.tier = target_tier;
		LLGET_TRACE(rargs->trace,
			    "reserve-hit tree=%lu target_tier=%u old_free=%u new_free=%u frame=0x%llx",
			    (unsigned long)idx, (unsigned)target_tier,
			    (unsigned)old_free, (unsigned)new_free,
			    (unsigned long long)res.frame);
	} else {
		if (rargs->trace != NULL && rargs->trace->enabled)
			rargs->trace->reserve_misses++;
		trees_unreserve(&self->trees, idx, old_free, target_tier, self->policy);
	}
	return res;
}

static llfree_result_t get_matching_reserve(llfree_t *self, uint8_t tier, size_t local, uint8_t order,
					    uint64_t start, llfree_get_trace_t *trace)
{
	llfree_result_t res;
	const size_t cl_trees = LLFREE_CACHE_SIZE / sizeof(tree_t);
	size_t near = self->trees.len / 16;
	if (near < cl_trees / 4)
		near = cl_trees / 4;
	start = align_down(start, next_pow2(2 * near));

	struct check_args check_args = {
		.tier = tier,
		.free = p_range(0, 0),
		.policy = self->policy,
	};
	reserve_args_t args = {
		.self = self,
		.order = order,
		.tier = tier,
		.local = local,
		.check_args = &check_args,
		.trace = trace,
	};

	if (order < LLFREE_HUGE_ORDER) {
		/* First: best-fit in nearby region, excluding fully-free trees */
		check_args.free = p_range((treeF_t)(1u << order), LLFREE_TREE_SIZE - 1);

		res = trees_search_best(&self->trees, tier, start, 1, near,
					(treeF_t)(1u << order), self->policy, get_reserve, &args);
		if (res.error != LLFREE_ERR_MEMORY)
			return res;

		/* Second: full scan, still excluding fully-free trees */
		res = trees_search(&self->trees, start, 1, self->trees.len, get_reserve, &args);
		if (res.error != LLFREE_ERR_MEMORY)
			return res;
	}

	/* Last resort: any tree including fully-free ones */
	check_args.free = p_range((treeF_t)(1u << order), LLFREE_TREE_SIZE);
	res = trees_search(&self->trees, start, 0, self->trees.len, get_reserve, &args);
	return res;
}

static bool sync_with_global(llfree_t *self, uint8_t tier, size_t index, treeF_t needed,
			     local_result_t old)
{
	if (old.free >= needed)
		return false;

	size_t tree_idx = tree_from_row(old.start_row);
	treeF_t steal_min = needed - old.free;

	treeF_t stolen;
	if (!trees_sync_steal(&self->trees, tree_idx, steal_min, &stolen))
		return false;

	if (!ll_local_put(self->local, tier, index, tree_idx, stolen)) {
		trees_put(&self->trees, tree_idx, stolen, self->policy);
		return false;
	}
	return true;
}

static llfree_result_t get_from_local(llfree_t *self, uint8_t tier, size_t index, uint8_t order,
				      treeF_t frames, local_result_t *old,
				      llfree_get_trace_t *trace)
{
	if (trace != NULL && trace->enabled)
		trace->local_attempts++;

	*old = ll_local_get(self->local, tier, index, ll_none(), frames);
	if (old->success) {
		llfree_result_t res =
			lower_get(&self->lower, frame_from_row(old->start_row), order, ll_none());
		if (llfree_is_ok(res)) {
			uint64_t start_row = row_from_frame(res.frame);
			if (old->start_row != start_row)
				*old = ll_local_set_start(self->local, tier, index, start_row);
			return llfree_ok(res.frame, tier);
		}
		/*
		 * ll_local_get already decremented this CPU's reservation; the global
		 * tree for that row is still reserved (free count lives in local).
		 * trees_put would inflate global free while reserved and can exceed
		 * LLFREE_TREE_SIZE (tree_put assert). Restore the local quota instead.
		 */
			if (!ll_local_put(self->local, tier, index,
					  tree_from_row(old->start_row), frames))
				llfree_assert(false);
			if (trace != NULL && trace->enabled)
				trace->local_misses++;
			return res;
		}

	if (old->present && sync_with_global(self, tier, index, frames, *old)) {
		if (trace != NULL && trace->enabled)
			trace->local_retries++;
		return llfree_err(LLFREE_ERR_RETRY);
	}

	if (trace != NULL && trace->enabled)
		trace->local_misses++;

	return llfree_err(LLFREE_ERR_MEMORY);
}

static llfree_result_t get_matching(llfree_t *self, const llfree_request_t *request, uint64_t *start,
				    llfree_get_trace_t *trace)
{
	size_t tier_count = ll_local_tier_locals(self->local, request->tier);
	treeF_t frames = (treeF_t)(1u << request->order);

	if (request->local != LLFREE_LOCAL_NONE && tier_count != LLFREE_LOCAL_NONE && tier_count > 0 &&
		tier_count < self->trees.len) {
		for (size_t i = 0; i < RETRIES; i++) {
			local_result_t old;
			llfree_result_t res =
				get_from_local(self, request->tier, request->local,
							request->order, frames, &old, trace);
			if (old.present)
				*start = tree_from_row(old.start_row);

		if (res.error == LLFREE_ERR_RETRY)
			continue;
		if (res.error == LLFREE_ERR_MEMORY)
			break;
		return res;
	}
		return get_matching_reserve(self, request->tier, request->local,
						request->order, *start, trace);
	}

	reserve_args_t args = { .self = self, .order = request->order, .tier = request->tier, .local = 0,
				.check_args = &(struct check_args){ .tier = request->tier,
					.free = p_range(frames, LLFREE_TREE_SIZE), .policy = self->policy },
				.trace = trace };
	return trees_search(&self->trees, *start, 0, self->trees.len, get_reserve, &args);
}

llfree_result_t llfree_get(llfree_t *self, ll_optional_t frame, llfree_request_t request)
{
	llfree_assert(self != NULL);
	llfree_assert(request.order <= LLFREE_MAX_ORDER);

#if LLFREE_TRACE_GET
	llfree_get_trace_t trace = { .enabled = true, .req_id = ll_get_trace_next_id() };
#else
	llfree_get_trace_t trace = { .enabled = false };
#endif

	if (frame.present) {
		/* get_at not needed in kernel use */
		llfree_result_t direct =
			lower_get(&self->lower, (uint64_t)frame.value, request.order,
				  ll_some(frame.value));
		LLGET_TRACE(&trace, "direct frame=0x%llx order=%u ret=%u",
			    (unsigned long long)frame.value, (unsigned)request.order,
			    (unsigned)direct.error);
		return direct;
	}

	size_t tier_count = ll_local_tier_locals(self->local, request.tier);
	uint64_t start;
	if (tier_count > 0 && request.local != LLFREE_LOCAL_NONE)
		start = self->trees.len / tier_count * request.local;
	else
		start = 0;

	LLGET_TRACE(&trace,
		    "start order=%u tier=%u local=%lu start_tree=%lu trees=%lu tier_locals=%lu",
		    (unsigned)request.order, (unsigned)request.tier,
		    (unsigned long)request.local, (unsigned long)start,
		    (unsigned long)self->trees.len, (unsigned long)tier_count);

	llfree_result_t res = get_matching(self, &request, &start, &trace);
	if (res.error != LLFREE_ERR_MEMORY) {
		LLGET_TRACE(&trace,
			    "done frame=0x%llx tree=%lu tier=%u local_attempts=%u local_miss=%u local_retry=%u reserve_attempts=%u reserve_miss=%u",
			    (unsigned long long)res.frame,
			    (unsigned long)tree_from_frame((size_t)res.frame),
			    (unsigned)res.tier, (unsigned)trace.local_attempts,
			    (unsigned)trace.local_misses, (unsigned)trace.local_retries,
			    (unsigned)trace.reserve_attempts,
			    (unsigned)trace.reserve_misses);
		return res;
	}

	LLGET_TRACE(&trace,
		    "oom local_attempts=%u local_miss=%u local_retry=%u reserve_attempts=%u reserve_miss=%u",
		    (unsigned)trace.local_attempts, (unsigned)trace.local_misses,
		    (unsigned)trace.local_retries, (unsigned)trace.reserve_attempts,
		    (unsigned)trace.reserve_misses);

	return llfree_err(LLFREE_ERR_MEMORY);
}

llfree_result_t llfree_put(llfree_t *self, uint64_t frame, llfree_request_t request)
{
	llfree_assert(self != NULL);
	llfree_assert(request.order <= LLFREE_MAX_ORDER);
	llfree_assert(frame < self->lower.frames);

	llfree_result_t res = lower_put(&self->lower, frame, request.order);
	if (!llfree_is_ok(res))
		return res;

	size_t tree_idx = tree_from_frame(frame);
	treeF_t frames = (treeF_t)(1u << request.order);

	if (request.local != LLFREE_LOCAL_NONE) {
		(void)ll_local_free_inc(self->local, request.tier, request.local, tree_idx);

		if (ll_local_put(self->local, request.tier, request.local, tree_idx, frames))
			return llfree_ok(0, 0);

		trees_put(&self->trees, tree_idx, frames, self->policy);
	} else {
		trees_put(&self->trees, tree_idx, frames, self->policy);
	}
	return llfree_ok(0, 0);
}

void llfree_drain(llfree_t *self)
{
	(void)self;
	/* Kernel path doesn't rely on drain currently */
}

size_t llfree_frames(const llfree_t *self)
{
	llfree_assert(self != NULL);
	return self->lower.frames;
}
