/* TODO: This file seems to be general */

#pragma once

#include <common/types.h>
#include <common/macro.h>

typedef u64 vmr_prop_t;
#define VMR_READ    (1 << 0)
#define VMR_WRITE   (1 << 1)
#define VMR_EXEC    (1 << 2)
#define VMR_DEVICE  (1 << 3)
#define VMR_NOCACHE (1 << 4)

/* functions */
int map_range_in_pgtbl(void *pgtbl, vaddr_t va, paddr_t pa, size_t len,
                       vmr_prop_t flags);
int unmap_range_in_pgtbl(void *pgtbl, vaddr_t va, size_t len);

#ifndef KBASE
#define KBASE 0xFFFFFF0000000000
#endif


#ifdef CHCORE

#define phys_to_virt(x) ((vaddr_t)((paddr_t)x + KBASE))

#ifndef KCODE
#define KCODE 0xFFFFFFFFC0000000
#endif

static inline paddr_t virt_to_phys(void *va)
{
	u64 x;
	x = (u64)va;
	BUG_ON((x < KBASE) || (x >= KCODE));

	return x - KBASE;
}

#endif
