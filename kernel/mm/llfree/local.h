#pragma once

#include <mm/llfree/llfree.h>
#include "tree.h"
#include "utils.h"

/*
 * Minimal local reservation implementation (adapted from llfree-c).
 * We keep the same algorithms but avoid stdatomic/_Atomic types.
 */

#define LAST_FREES 4U

typedef struct local_result {
	bool success;
	bool present;
	uint8_t tier;
	treeF_t free;
	uint64_t start_row;
} local_result_t;

typedef struct demote_any_result {
	bool found;
	uint64_t row;
	bool old_present;
	uint64_t old_row;
	uint8_t old_tier;
	treeF_t old_free;
} demote_any_result_t;

typedef struct local local_t;

size_t ll_local_size(const llfree_tiering_t *tiering);
void ll_local_init(local_t *self, const llfree_tiering_t *tiering);
size_t ll_local_tier_locals(const local_t *self, uint8_t tier);
size_t ll_local_mem_size(const local_t *self);

local_result_t ll_local_get(local_t *self, uint8_t tier, size_t index, ll_optional_t tree_idx,
			    treeF_t frames);
bool ll_local_put(local_t *self, uint8_t tier, size_t index, size_t tree_idx, treeF_t frames);
local_result_t ll_local_set_start(local_t *self, uint8_t tier, size_t index, uint64_t start_row);
local_result_t ll_local_swap(local_t *self, uint8_t tier, size_t index, size_t new_tree_idx,
			     treeF_t new_free);

local_result_t ll_local_steal(local_t *self, uint8_t tier, size_t index, ll_optional_t tree_idx,
			      treeF_t frames, llfree_policy_fn policy);

demote_any_result_t ll_local_demote_any(local_t *self, uint8_t tier, size_t index,
					ll_optional_t tree_idx, treeF_t frames,
					llfree_policy_fn policy);

bool ll_local_free_inc(local_t *self, uint8_t tier, size_t index, size_t tree_idx);
local_result_t ll_local_drain(local_t *self, uint8_t tier, size_t index);

