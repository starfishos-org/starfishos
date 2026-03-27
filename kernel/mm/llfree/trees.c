#include "trees.h"

void trees_init(trees_t *self, size_t frames, uint8_t *buffer, trees_init_fn init_fn, void *init_ctx,
		uint8_t default_tier)
{
	self->len = div_ceil(frames, LLFREE_TREE_SIZE);
	self->entries = (tree_t *)buffer;
	self->default_tier = default_tier;

	if (init_fn != NULL) {
		for (size_t i = 0; i < self->len; ++i) {
			treeF_t free = init_fn(i * LLFREE_TREE_SIZE, init_ctx);
			self->entries[i] = tree_new(false, default_tier, free);
		}
	}
}

bool trees_get(trees_t *self, size_t idx, treeF_t frames, tree_check_fn check, void *args,
	       uint8_t *out_tier)
{
	llfree_assert(idx < self->len);
	tree_t old;
	return atom_update(&self->entries[idx], old, tree_get, frames, out_tier, check, args);
}

void trees_put(trees_t *self, size_t idx, treeF_t frames, llfree_policy_fn policy)
{
	llfree_assert(idx < self->len);
	tree_t old;
	atom_update(&self->entries[idx], old, tree_put, frames, policy, self->default_tier);
}

bool trees_reserve(trees_t *self, size_t idx, tree_check_fn check, void *args, treeF_t *out_free,
		   uint8_t *out_tier)
{
	llfree_assert(idx < self->len);
	tree_t old;
	bool ok = atom_update(&self->entries[idx], old, tree_reserve, out_tier, check, args);
	if (ok && out_free != NULL)
		*out_free = old.free;
	return ok;
}

void trees_unreserve(trees_t *self, size_t idx, treeF_t free, uint8_t tier, llfree_policy_fn policy)
{
	llfree_assert(idx < self->len);
	tree_t old;
	atom_update(&self->entries[idx], old, tree_unreserve_add, free, tier, policy, self->default_tier);
}

bool trees_sync_steal(trees_t *self, size_t idx, treeF_t min, treeF_t *out_stolen)
{
	llfree_assert(idx < self->len);
	tree_t old;
	bool ok = atom_update(&self->entries[idx], old, tree_sync_steal, min);
	if (ok && out_stolen != NULL)
		*out_stolen = old.free;
	return ok;
}

llfree_result_t trees_search(const trees_t *self, size_t start, size_t offset, size_t len,
			     trees_access_fn cb, void *ctx)
{
	s64 base = (s64)(start + self->len);
	for (s64 i = (s64)offset; i < (s64)len; ++i) {
		s64 off = i % 2 == 0 ? i / 2 : -((i + 1) / 2);
		size_t idx = (size_t)(base + off) % self->len;
		llfree_result_t res = cb(idx, ctx);
		if (res.error != LLFREE_ERR_MEMORY)
			return res;
	}
	return llfree_err(LLFREE_ERR_MEMORY);
}

llfree_result_t trees_search_best(const trees_t *self, uint8_t tier, size_t start, size_t offset,
				  size_t len, treeF_t min_free, llfree_policy_fn policy,
				  trees_access_fn cb, void *ctx)
{
	struct best {
		uint8_t prio;
		size_t idx;
	};
	struct best best[TREES_SEARCH_BEST] = { { 0 } };

	s64 base = (s64)(start + self->len);
	for (s64 i = (s64)offset; i < (s64)len; ++i) {
		s64 off = i % 2 == 0 ? i / 2 : -((i + 1) / 2);
		size_t idx = (size_t)(base + off) % self->len;

		tree_t tree = atom_load(&self->entries[idx]);
		if (tree.reserved || tree.free < min_free || tree.free == LLFREE_TREE_SIZE)
			continue;

		llfree_policy_t p = policy(tier, tree.tier, tree.free);
		if (p.type != LLFREE_POLICY_MATCH)
			continue;

		/* Perfect match: try immediately */
		if (p.priority == UINT8_MAX) {
			llfree_result_t res = cb(idx, ctx);
			if (res.error != LLFREE_ERR_MEMORY)
				return res;
			continue;
		}

		/* Priority+1 so 0 means "no candidate" */
		uint8_t prio = p.priority + 1;

		size_t pos = 0;
		for (; pos < TREES_SEARCH_BEST; ++pos) {
			if (prio > best[pos].prio)
				break;
		}
		if (pos < TREES_SEARCH_BEST) {
			for (size_t j = TREES_SEARCH_BEST - 1; j > pos; --j)
				best[j] = best[j - 1];
			best[pos].prio = prio;
			best[pos].idx = idx;
		}
	}

	for (size_t i = 0; i < TREES_SEARCH_BEST; ++i) {
		if (best[i].prio == 0)
			break;
		llfree_result_t res = cb(best[i].idx, ctx);
		if (res.error != LLFREE_ERR_MEMORY)
			return res;
	}

	return llfree_err(LLFREE_ERR_MEMORY);
}

