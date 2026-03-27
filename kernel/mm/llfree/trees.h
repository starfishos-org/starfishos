#pragma once

#include <mm/llfree/llfree.h>
#include "tree.h"
#include "utils.h"

typedef struct trees {
	tree_t *entries; /* atomics via llfree_platform.h */
	size_t len;
	uint8_t default_tier;
} trees_t;

static inline ll_unused size_t trees_metadata_size(size_t frames)
{
	size_t tree_len = div_ceil(frames, LLFREE_TREE_SIZE);
	return align_up(sizeof(tree_t) * tree_len, LLFREE_CACHE_SIZE);
}

typedef treeF_t (*trees_init_fn)(size_t tree_start_frame, void *ctx);

void trees_init(trees_t *self, size_t frames, uint8_t *buffer, trees_init_fn init_fn,
		void *init_ctx, uint8_t default_tier);

bool trees_get(trees_t *self, size_t idx, treeF_t frames, tree_check_fn check, void *args,
	       uint8_t *out_tier);
void trees_put(trees_t *self, size_t idx, treeF_t frames, llfree_policy_fn policy);
bool trees_reserve(trees_t *self, size_t idx, tree_check_fn check, void *args, treeF_t *out_free,
		   uint8_t *out_tier);
void trees_unreserve(trees_t *self, size_t idx, treeF_t free, uint8_t tier, llfree_policy_fn policy);
bool trees_sync_steal(trees_t *self, size_t idx, treeF_t min, treeF_t *out_stolen);

#define TREES_MIN_FREE (LLFREE_TREE_SIZE / 16)
#define TREES_SEARCH_BEST 3

typedef llfree_result_t (*trees_access_fn)(size_t idx, void *ctx);
llfree_result_t trees_search(const trees_t *self, size_t start, size_t offset, size_t len,
			     trees_access_fn cb, void *ctx);
llfree_result_t trees_search_best(const trees_t *self, uint8_t tier, size_t start, size_t offset,
				  size_t len, treeF_t min_free, llfree_policy_fn policy,
				  trees_access_fn cb, void *ctx);

