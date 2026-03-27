#include "tree.h"

bool tree_put(tree_t *self, treeF_t frames, llfree_policy_fn policy, uint8_t default_tier)
{
	treeF_t free = self->free + frames;
	llfree_assert(free <= LLFREE_TREE_SIZE);
	self->free = free;
	if (free == LLFREE_TREE_SIZE &&
	    policy(self->tier, default_tier, self->free).type != LLFREE_POLICY_INVALID)
		self->tier = default_tier;
	return true;
}

bool tree_get(tree_t *self, treeF_t frames, uint8_t *result_tier, tree_check_fn check, void *args)
{
	if (self->free < frames)
		return false;
	uint8_t new_tier = check(self->tier, self->free, args);
	if (new_tier == LLFREE_TIER_NONE)
		return false;

	self->free -= frames;
	self->tier = new_tier;
	if (result_tier != NULL)
		*result_tier = new_tier;
	return true;
}

bool tree_reserve(tree_t *self, uint8_t *result_tier, tree_check_fn check, void *args)
{
	llfree_assert(check != NULL);
	if (self->reserved)
		return false;
	uint8_t new_tier = check(self->tier, self->free, args);
	if (new_tier == LLFREE_TIER_NONE)
		return false;

	self->reserved = true;
	self->tier = new_tier;
	self->free = 0;
	if (result_tier != NULL)
		*result_tier = new_tier;
	return true;
}

bool tree_unreserve_add(tree_t *self, treeF_t frames, uint8_t tier, llfree_policy_fn policy,
			uint8_t default_tier)
{
	if (!self->reserved)
		return false;
	self->reserved = false;
	llfree_policy_t p = policy(tier, self->tier, frames);
	if (p.type == LLFREE_POLICY_DEMOTE)
		self->tier = tier;
	return tree_put(self, frames, policy, default_tier);
}

bool tree_sync_steal(tree_t *self, treeF_t min)
{
	if (self->reserved && self->free > min) {
		self->free = 0;
		return true;
	}
	return false;
}

