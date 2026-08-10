#pragma once

#include <common/types.h>

typedef u64 pmo_type_t;

/* TODO(FN): move to uapi.h */
#define PMO_ANONYM            0 /* lazy allocation */
#define PMO_DATA              1 /* immediate allocation */
#define PMO_FILE              2 /* file backed */
#define PMO_SHM               3 /* shared memory */
#define PMO_USER_PAGER        4 /* support user pager */
#define PMO_DEVICE            5 /* memory mapped device registers */
#define PMO_DATA_NOCACHE      6 /* non-cacheable immediate allocation */
#define PMO_FORBID            7 /* forbidden area: avoid overflow */

/* Types below reuse the storage behavior of an earlier type. */
#define PMO_RING_BUFFER       8 /* externally synchronized, PMO_DATA */
#define PMO_RING_BUFFER_RADIX 9 /* test variant, PMO_ANONYM */
#define PMO_CODE              10 /* code, PMO_DATA */
#define PMO_STACK             11 /* stack, PMO_ANONYM */
#define PMO_HEAP              12 /* heap, PMO_ANONYM */
#define PMO_CROSS_SHM         14 /* cross-machine shared memory, PMO_SHM */
#define PMO_TYPE_NR           15
