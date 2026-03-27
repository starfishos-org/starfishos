#pragma once

#include <common/types.h>
#include <common/macro.h>
#include <mm/llfree/llfree_types.h>
#include "llfree_platform.h"

static inline ll_unused size_t div_ceil(size_t a, size_t b)
{
	return (a + b - 1) / b;
}

static inline ll_unused size_t align_up(size_t x, size_t a)
{
	return ROUND_UP(x, a);
}

static inline ll_unused size_t align_down(size_t x, size_t a)
{
	return x & ~(a - 1);
}

static inline ll_unused size_t next_pow2(size_t v)
{
	if (v <= 1)
		return 1;
	v--;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
#if __SIZEOF_SIZE_T__ == 8
	v |= v >> 32;
#endif
	return v + 1;
}

#define LL_MAX(a, b) ((a) > (b) ? (a) : (b))
#define LL_MIN(a, b) ((a) < (b) ? (a) : (b))

/*
 * Iteration helper used by llfree-c: iterate `len` values within the
 * len-sized bucket containing `start`, beginning at `start` then wrapping
 * inside that bucket.
 */
#define for_offsetted(start, len, var)                                            \
	for (size_t _i = 0, _offset = (start) % (len),                            \
		    _base_idx = (start) - _offset, (var) = (start);              \
	     _i < (len);                                                         \
	     _i = _i + 1, (var) = _base_idx + ((_i + _offset) % (len)))

/* frame <-> tree/row helpers */
static inline ll_unused size_t tree_from_frame(size_t frame)
{
	return frame / LLFREE_TREE_SIZE;
}

static inline ll_unused uint64_t frame_from_tree(size_t tree)
{
	return (uint64_t)(tree * LLFREE_TREE_SIZE);
}

static inline ll_unused uint64_t row_from_tree(size_t tree)
{
	return frame_from_tree(tree) / LLFREE_ATOMIC_SIZE;
}

static inline ll_unused size_t child_from_frame(uint64_t frame)
{
	return (size_t)(frame / LLFREE_CHILD_SIZE);
}

static inline ll_unused uint64_t frame_from_child(size_t child)
{
	return (uint64_t)(child * (uint64_t)LLFREE_CHILD_SIZE);
}

static inline ll_unused uint64_t row_from_frame(uint64_t frame)
{
	return frame / LLFREE_ATOMIC_SIZE;
}

static inline ll_unused uint64_t frame_from_row(uint64_t row)
{
	return row * LLFREE_ATOMIC_SIZE;
}

static inline ll_unused size_t tree_from_row(uint64_t row)
{
	return (size_t)(frame_from_row(row) / LLFREE_TREE_SIZE);
}

/* Bounds */
#define MIN_PAGES (LLFREE_TREE_SIZE)
#define MAX_PAGES ((size_t)1 << 28)

#define INDENT(n) ((n) == 0 ? "" : ((n) == 1 ? "  " : "    "))

#if 0
/* Duplicate legacy content kept for reference (disabled). */
#pragma once
#include <common/types.h>
#include <common/macro.h>
#include <mm/llfree/llfree_types.h>
#include "llfree_platform.h"
static inline ll_unused size_t div_ceil(size_t a, size_t b) { return (a + b - 1) / b; }
static inline ll_unused size_t align_up(size_t x, size_t a) { return ROUND_UP(x, a); }
static inline ll_unused size_t align_down(size_t x, size_t a) { return x & ~(a - 1); }
static inline ll_unused size_t next_pow2(size_t v) { return v; }
static inline ll_unused size_t ll_max(size_t a, size_t b) { return a > b ? a : b; }
#define LL_MAX(a, b) ((a) > (b) ? (a) : (b))
static inline ll_unused size_t tree_from_frame(size_t frame) { return frame / LLFREE_TREE_SIZE; }
static inline ll_unused uint64_t frame_from_tree(size_t tree) { return (uint64_t)(tree * LLFREE_TREE_SIZE); }
static inline ll_unused uint64_t row_from_frame(uint64_t frame) { return frame / LLFREE_ATOMIC_SIZE; }
static inline ll_unused uint64_t frame_from_row(uint64_t row) { return row * LLFREE_ATOMIC_SIZE; }
static inline ll_unused size_t tree_from_row(uint64_t row) { return (size_t)(frame_from_row(row) / LLFREE_TREE_SIZE); }
static inline ll_unused uint64_t tree_from_frame_u64(uint64_t frame) { return frame / LLFREE_TREE_SIZE; }
#define MIN_PAGES (LLFREE_TREE_SIZE)
#define MAX_PAGES ((size_t)1 << 28)
#define INDENT(n) ((n) == 0 ? "" : ((n) == 1 ? "  " : "    "))
#endif
