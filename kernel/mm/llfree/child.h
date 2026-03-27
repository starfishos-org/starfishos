#pragma once

#include <mm/llfree/llfree.h>
#include "utils.h"

/*
 * Use 32-bit child_t so we can use ChCore's 32-bit CAS primitives.
 * free needs to cover up to LLFREE_CHILD_SIZE (<= 2^31 here).
 */
#define CHILD_FREE_BITS 31
_Static_assert((1u << LLFREE_CHILD_ORDER) <= (1u << CHILD_FREE_BITS), "child counter size");

typedef struct child {
	u32 free : CHILD_FREE_BITS;
	u32 huge : 1;
} child_t;
_Static_assert(sizeof(child_t) == sizeof(u32), "child size mismatch");

static inline child_t ll_unused child_new(uint16_t free, bool huge)
{
	llfree_assert(free <= LLFREE_CHILD_SIZE);
	llfree_assert(!huge || free == 0);
	return (child_t){ .free = (u32)free, .huge = huge ? 1u : 0u };
}

bool child_inc(child_t *self, size_t order);
bool child_dec(child_t *self, size_t order);
bool child_set_huge(child_t *self);
bool child_clear_huge(child_t *self);

typedef struct child_pair {
	child_t first, second;
} child_pair_t;

bool child_set_max(child_pair_t *self);
bool child_clear_max(child_pair_t *self);

