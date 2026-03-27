#pragma once

#include <common/types.h>

/*
 * Kernel-adapted llfree types/config.
 * This is derived from linux-tests/llfree-c/std/llfree_types.h but avoids
 * libc headers (kernel builds with -nostdinc).
 */

/* Number of Bytes in cacheline */
#define LLFREE_CACHE_SIZE 64u

/* Base frame: 4KiB */
#define LLFREE_FRAME_BITS 12u
#define LLFREE_FRAME_SIZE (1u << LLFREE_FRAME_BITS)

/* Huge frame order: 2MiB (512 * 4KiB) */
#define LLFREE_HUGE_ORDER 9u
/* Maximum order that can be allocated (4MiB) */
#define LLFREE_MAX_ORDER (LLFREE_HUGE_ORDER + 1u)

/* Largest atomic width in bytes (x86_64: 8B) */
#define LLFREE_ATOMIC_ORDER 6u
#define LLFREE_ATOMIC_SIZE (1u << LLFREE_ATOMIC_ORDER)

/* Frames in a child */
#define LLFREE_CHILD_ORDER LLFREE_HUGE_ORDER
#define LLFREE_CHILD_SIZE  (1u << LLFREE_CHILD_ORDER)

/* Frames in a tree */
#define LLFREE_TREE_CHILDREN_ORDER 3u
#define LLFREE_TREE_CHILDREN       (1u << LLFREE_TREE_CHILDREN_ORDER)
#define LLFREE_TREE_ORDER          (LLFREE_HUGE_ORDER + LLFREE_TREE_CHILDREN_ORDER)
#define LLFREE_TREE_SIZE           (1u << LLFREE_TREE_ORDER)

/* Enable reserve-on-free heuristic */
#define LLFREE_ENABLE_FREE_RESERVE false
/* Prefer already-installed huge frames */
#define LLFREE_PREFER_INSTALLED false

/*
 * Minimal alignment required for the managed memory range.
 * = (1 << LLFREE_MAX_ORDER) * frame_size
 */
#define LLFREE_ALIGN (1ULL << LLFREE_MAX_ORDER << LLFREE_FRAME_BITS)

/* Tree tier bits */
#define LLFREE_TIER_BITS 3u
#define LLFREE_TIER_NONE ((u8)0xFF)
#define LLFREE_MAX_TIERS (1ULL << LLFREE_TIER_BITS)

/* Provide stdint-like aliases expected by llfree code */
typedef u64 uint64_t;
typedef u32 uint32_t;
typedef u16 uint16_t;
typedef u8  uint8_t;

#ifndef UINT64_MAX
#define UINT64_MAX (~0ULL)
#endif
#ifndef UINT32_MAX
#define UINT32_MAX (~0U)
#endif
#ifndef UINT8_MAX
#define UINT8_MAX ((u8)0xFF)
#endif
