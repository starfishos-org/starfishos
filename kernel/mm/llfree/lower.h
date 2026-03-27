#pragma once

#include <mm/llfree/llfree.h>
#include "bitfield.h"
#include "child.h"

typedef struct children {
	child_t entries[LLFREE_TREE_CHILDREN] ll_align(LLFREE_CACHE_SIZE);
} children_t;

typedef struct lower {
	size_t frames;
	bitfield_t *fields;
	children_t *children;
} lower_t;

llfree_result_t lower_init(lower_t *self, size_t frames, uint8_t init, uint8_t *primary);
size_t lower_metadata_size(size_t frames);
uint8_t *lower_metadata(const lower_t *self);
llfree_result_t lower_get(lower_t *self, uint64_t start_frame, size_t order, ll_optional_t frame);
llfree_result_t lower_put(lower_t *self, uint64_t frame, size_t order);

