#pragma once

#include <mm/llfree/llfree.h>
#include "trees.h"
#include "local.h"
#include "lower.h"

typedef struct __attribute__((aligned(LLFREE_CACHE_SIZE))) llfree {
	lower_t lower;
	local_t *local;
	trees_t trees;
	llfree_policy_fn policy;
	uint8_t num_tiers;
} llfree_t;

