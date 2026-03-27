#pragma once

#include <mm/llfree/llfree.h>
#include "utils.h"

typedef uint32_t treeF_t;

#define LLFREE_TREE_FREE_BITS ((8 * sizeof(treeF_t)) - 1 - LLFREE_TIER_BITS)
_Static_assert((1u << LLFREE_TREE_FREE_BITS) > LLFREE_TREE_SIZE, "Tree free counter too small");

typedef struct tree {
	bool reserved : 1;
	uint8_t tier : LLFREE_TIER_BITS;
	treeF_t free : LLFREE_TREE_FREE_BITS;
} tree_t;
_Static_assert(sizeof(tree_t) == sizeof(treeF_t), "tree size mismatch");

typedef uint8_t (*tree_check_fn)(uint8_t tree_tier, treeF_t frames, void *args);

static inline tree_t ll_unused tree_new(bool reserved, uint8_t tier, treeF_t free)
{
	llfree_assert(free <= LLFREE_TREE_SIZE);
	llfree_assert(tier < LLFREE_MAX_TIERS);
	return (tree_t){ .reserved = reserved, .tier = tier, .free = free };
}

bool tree_put(tree_t *self, treeF_t frames, llfree_policy_fn policy, uint8_t default_tier);
bool tree_get(tree_t *self, treeF_t frames, uint8_t *result_tier, tree_check_fn check, void *args);
bool tree_reserve(tree_t *self, uint8_t *result_tier, tree_check_fn check, void *args);
bool tree_unreserve_add(tree_t *self, treeF_t frames, uint8_t tier, llfree_policy_fn policy,
			uint8_t default_tier);
bool tree_sync_steal(tree_t *self, treeF_t min);

