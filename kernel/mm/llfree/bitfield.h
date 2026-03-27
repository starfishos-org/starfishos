#pragma once

#include <mm/llfree/llfree.h>
#include <mm/llfree/llfree_types.h>
#include "llfree_platform.h"
#include "utils.h"

#define FIELD_N (LLFREE_CHILD_SIZE / LLFREE_ATOMIC_SIZE)

typedef struct bitfield {
	u64 rows[FIELD_N] ll_align(LLFREE_CACHE_SIZE);
} bitfield_t;

void field_init(bitfield_t *self);
llfree_result_t field_set_next(bitfield_t *field, uint64_t start_frame, size_t order);
llfree_result_t field_toggle(bitfield_t *field, size_t index, size_t order, bool expected);
size_t field_count_ones(bitfield_t *field);
bool field_is_free(bitfield_t *self, size_t index);

