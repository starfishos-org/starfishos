#include "local.h"
#include "llfree_platform.h"

typedef struct reserved {
	bool present : 1;
	treeF_t free : LLFREE_TREE_FREE_BITS;
	uint64_t start_row : 64 - LLFREE_TREE_FREE_BITS - 1;
} reserved_t;
_Static_assert(sizeof(reserved_t) == sizeof(uint64_t), "size overflow");

typedef struct local_history {
	uint64_t idx : 48;
	uint16_t frees : 16;
} local_history_t;
_Static_assert(sizeof(local_history_t) == sizeof(uint64_t), "size overflow");

static inline reserved_t ll_reserved_new(bool present, treeF_t free, uint64_t start_row)
{
	llfree_assert(free <= LLFREE_TREE_SIZE);
	return (reserved_t){ present, free, start_row };
}

static bool ll_reserved_dec(reserved_t *self, ll_optional_t tree_idx, treeF_t frames)
{
	if (!self->present)
		return false;
	if (tree_idx.present && tree_from_row(self->start_row) != tree_idx.value)
		return false;
	if (self->free < frames)
		return false;
	self->free -= frames;
	return true;
}

static bool ll_reserved_inc(reserved_t *self, size_t tree_idx, treeF_t frames)
{
	if (!self->present || tree_from_row(self->start_row) != tree_idx)
		return false;
	treeF_t free = self->free + frames;
	llfree_assert(free <= LLFREE_TREE_SIZE);
	self->free = free;
	return true;
}

static bool ll_reserved_swap(reserved_t *self, reserved_t newv)
{
	*self = newv;
	return true;
}

static bool ll_reserved_set_start(reserved_t *self, uint64_t start_row)
{
	if (!self->present || tree_from_row(self->start_row) != tree_from_row(start_row))
		return false;
	self->start_row = start_row;
	return true;
}

static bool ll_reserved_take(reserved_t *self)
{
	if (!self->present)
		return false;
	*self = ll_reserved_new(false, 0, 0);
	return true;
}

typedef struct entry {
	reserved_t preferred ll_align(LLFREE_CACHE_SIZE);
	local_history_t last ll_align(LLFREE_CACHE_SIZE);
} entry_t;

typedef struct tier_locals {
	entry_t *entries;
	size_t len;
} tier_locals_t;

typedef struct local {
	size_t len;
	uint8_t num_tiers;
	tier_locals_t tiers[LLFREE_MAX_TIERS];
} local_t;

size_t ll_local_size(const llfree_tiering_t *tiering)
{
	size_t total = 0;
	for (size_t i = 0; i < tiering->num_tiers; i++)
		total += tiering->tiers[i].count;
	return align_up(sizeof(local_t), LLFREE_CACHE_SIZE) + sizeof(entry_t) * total;
}

void ll_local_init(local_t *self, const llfree_tiering_t *tiering)
{
	llfree_assert(self != NULL);
	llfree_assert(((size_t)self % LLFREE_CACHE_SIZE) == 0);

	size_t total = 0;
	for (size_t i = 0; i < tiering->num_tiers; i++)
		total += tiering->tiers[i].count;
	self->len = total;
	self->num_tiers = (uint8_t)tiering->num_tiers;

	entry_t *base = (entry_t *)((uint8_t *)self + align_up(sizeof(local_t), LLFREE_CACHE_SIZE));

	for (size_t i = 0; i < LLFREE_MAX_TIERS; i++)
		self->tiers[i] = (tier_locals_t){ .entries = NULL, .len = 0 };

	size_t offset = 0;
	for (size_t i = 0; i < tiering->num_tiers; i++) {
		uint8_t tier = tiering->tiers[i].tier;
		size_t count = tiering->tiers[i].count;
		self->tiers[tier] = (tier_locals_t){ .entries = &base[offset], .len = count };
		for (size_t j = 0; j < count; j++) {
			entry_t *e = &base[offset + j];
			atom_store(&e->preferred, ll_reserved_new(false, 0, 0));
			atom_store(&e->last, ((local_history_t){ 0, 0 }));
		}
		offset += count;
	}
}

size_t ll_local_tier_locals(const local_t *self, uint8_t tier)
{
	if (tier >= LLFREE_MAX_TIERS || self->tiers[tier].entries == NULL)
		return LLFREE_LOCAL_NONE;
	return self->tiers[tier].len;
}

size_t ll_local_mem_size(const local_t *self)
{
	return align_up(sizeof(local_t), LLFREE_CACHE_SIZE) + sizeof(entry_t) * self->len;
}

static inline local_result_t make_result(bool success, uint8_t tier, reserved_t old)
{
	return (local_result_t){
		.success = success,
		.present = old.present,
		.tier = tier,
		.free = old.free,
		.start_row = old.start_row,
	};
}

local_result_t ll_local_get(local_t *self, uint8_t tier, size_t index, ll_optional_t tree_idx,
			    treeF_t frames)
{
	llfree_assert(tier < LLFREE_MAX_TIERS);
	llfree_assert(index < self->tiers[tier].len);
	entry_t *entry = &self->tiers[tier].entries[index];
	reserved_t old;
	bool ok = atom_update(&entry->preferred, old, ll_reserved_dec, tree_idx, frames);
	return make_result(ok, tier, old);
}

bool ll_local_put(local_t *self, uint8_t tier, size_t index, size_t tree_idx, treeF_t frames)
{
	llfree_assert(tier < LLFREE_MAX_TIERS);
	llfree_assert(index < self->tiers[tier].len);
	entry_t *entry = &self->tiers[tier].entries[index];
	reserved_t old;
	return atom_update(&entry->preferred, old, ll_reserved_inc, tree_idx, frames);
}

local_result_t ll_local_set_start(local_t *self, uint8_t tier, size_t index, uint64_t start_row)
{
	llfree_assert(tier < LLFREE_MAX_TIERS);
	llfree_assert(index < self->tiers[tier].len);
	entry_t *entry = &self->tiers[tier].entries[index];
	reserved_t old;
	bool ok = atom_update(&entry->preferred, old, ll_reserved_set_start, start_row);
	return make_result(ok, tier, old);
}

local_result_t ll_local_swap(local_t *self, uint8_t tier, size_t index, size_t new_tree_idx,
			     treeF_t new_free)
{
	llfree_assert(tier < LLFREE_MAX_TIERS);
	llfree_assert(index < self->tiers[tier].len);
	entry_t *entry = &self->tiers[tier].entries[index];
	reserved_t newv = ll_reserved_new(true, new_free, row_from_tree(new_tree_idx));
	reserved_t old;
	atom_update(&entry->preferred, old, ll_reserved_swap, newv);
	return make_result(true, tier, old);
}

local_result_t ll_local_steal(local_t *self, uint8_t tier, size_t index, ll_optional_t tree_idx,
			      treeF_t frames, llfree_policy_fn policy)
{
	for (size_t i = 0; i < LLFREE_MAX_TIERS; i++) {
		uint8_t target_tier = (uint8_t)((i + tier) % LLFREE_MAX_TIERS);
		tier_locals_t *target = &self->tiers[target_tier];
		if (target->len == 0)
			continue;

		llfree_policy_t p = policy(tier, target_tier, (size_t)frames);
		if (p.type != LLFREE_POLICY_MATCH && p.type != LLFREE_POLICY_STEAL)
			continue;

		for (size_t j = 0; j < target->len; j++) {
			size_t jj = (index + j) % target->len;
			reserved_t old;
			bool ok = atom_update(&target->entries[jj].preferred, old, ll_reserved_dec,
					      tree_idx, frames);
			if (ok)
				return make_result(true, target_tier, old);
		}
	}
	return (local_result_t){ .success = false };
}

demote_any_result_t ll_local_demote_any(local_t *self, uint8_t tier, size_t index,
					ll_optional_t tree_idx, treeF_t frames,
					llfree_policy_fn policy)
{
	demote_any_result_t fail = { .found = false };

	for (uint8_t i = 1; i < LLFREE_MAX_TIERS; i++) {
		uint8_t target_tier = (uint8_t)((i + tier) % LLFREE_MAX_TIERS);
		tier_locals_t *target = &self->tiers[target_tier];
		if (target->len == 0)
			continue;

		llfree_policy_t p = policy(tier, target_tier, (size_t)frames);
		if (p.type != LLFREE_POLICY_DEMOTE)
			continue;

		for (size_t j = 0; j < target->len; j++) {
			size_t jj = (index + j) % target->len;

			reserved_t old;
			bool ok = atom_update(&target->entries[jj].preferred, old, ll_reserved_take);
			if (!ok)
				continue;

			if (old.free < frames ||
			    (tree_idx.present && tree_from_row(old.start_row) != tree_idx.value)) {
				reserved_t restore = old;
				atom_update(&target->entries[jj].preferred, old, ll_reserved_swap,
					    restore);
				continue;
			}

			reserved_t new_res = ll_reserved_new(true, old.free - frames, old.start_row);

			demote_any_result_t result = { .found = true, .row = old.start_row };
			tier_locals_t *req = &self->tiers[tier];
			if (req->len > 0 && index < req->len) {
				reserved_t prev;
				atom_update(&req->entries[index].preferred, prev, ll_reserved_swap,
					    new_res);
				result.old_present = prev.present;
				result.old_row = prev.start_row;
				result.old_tier = tier;
				result.old_free = prev.free;
			} else {
				result.old_present = false;
			}
			return result;
		}
	}
	return fail;
}

static bool frees_inc(local_history_t *self, size_t tree_idx)
{
	if (self->idx != tree_idx) {
		self->idx = tree_idx;
		self->frees = 1;
		return true;
	}
	if (self->frees < LAST_FREES) {
		self->frees += 1;
		return true;
	}
	return false;
}

bool ll_local_free_inc(local_t *self, uint8_t tier, size_t index, size_t tree_idx)
{
	llfree_assert(tier < LLFREE_MAX_TIERS);
	llfree_assert(index < self->tiers[tier].len);
	entry_t *entry = &self->tiers[tier].entries[index];
	local_history_t frees;
	bool updated = atom_update(&entry->last, frees, frees_inc, tree_idx);
	return !updated;
}

local_result_t ll_local_drain(local_t *self, uint8_t tier, size_t index)
{
	llfree_assert(tier < LLFREE_MAX_TIERS);
	llfree_assert(index < self->tiers[tier].len);
	entry_t *entry = &self->tiers[tier].entries[index];
	reserved_t old;
	atom_update(&entry->preferred, old, ll_reserved_swap, ll_reserved_new(false, 0, 0));
	return make_result(old.present, tier, old);
}

