#pragma once

#include <chcore/type.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Allocate and free virtual address region.
 * The size of allocated vaddr block is aligned to 4K.
 */
u64 chcore_alloc_vaddr(u64 size);
void chcore_free_vaddr(u64 vaddr, u64 size);

/*
 * Automatically map and unmap PMO to available virtual address.
 */
void *chcore_auto_map_pmo(int pmo, u64 size, u64 perm);
void chcore_auto_unmap_pmo(int pmo, u64 vaddr, u64 size);

struct chcore_dma_handle {
	int pmo;
	u64 size;
	u64 vaddr;
	u64 paddr;
};
/*
 * Allocate and free coherent memory (consecutive & non-cacheable) for DMA.
 */
void *chcore_alloc_dma_mem(u64 size, struct chcore_dma_handle *dma_handle);
void chcore_free_dma_mem(struct chcore_dma_handle *dma_handle);

#ifdef __cplusplus
}
#endif
