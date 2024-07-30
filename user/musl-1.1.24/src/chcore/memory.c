#include <chcore/memory.h>

#include <chcore/defs.h>
#include <chcore/syscall.h>
#include <chcore/bug.h>

#define MEM_AUTO_ALLOC_REGION      0x300000000000UL
#define MEM_AUTO_ALLOC_REGION_SIZE 0x100000000000UL
// TODO: if this module is used widely in the future, the size may need to be increased

u64 chcore_alloc_vaddr(u64 size)
{
	static u64 current = MEM_AUTO_ALLOC_REGION;

	size = ROUND_UP(size, PAGE_SIZE);
	if (size == 0)
		return 0;

	u64 allocated = __sync_fetch_and_add(&current, size);
	if (allocated >= MEM_AUTO_ALLOC_REGION + MEM_AUTO_ALLOC_REGION_SIZE)
		return 0;
	return allocated;
}

void chcore_free_vaddr(u64 vaddr, u64 size)
{
	// TODO
}

void *chcore_auto_map_pmo(int pmo, u64 size, u64 perm)
{
	u64 vaddr = chcore_alloc_vaddr(size);
	if (vaddr == 0)
		return NULL;
	int ret = usys_map_pmo(SELF_CAP, pmo, vaddr, perm);
	if (ret != 0) {
		chcore_free_vaddr(vaddr, size);
		return NULL;
	}
	return (void *)vaddr;
}

void chcore_auto_unmap_pmo(int pmo, u64 vaddr, u64 size)
{
	usys_unmap_pmo(SELF_CAP, pmo, vaddr);
	chcore_free_vaddr(vaddr, size);
}

void *chcore_alloc_dma_mem(u64 size, struct chcore_dma_handle *dma_handle)
{
	int ret;

	if (dma_handle == NULL) {
		return NULL;
	}

	dma_handle->size = size;

	dma_handle->pmo = usys_create_pmo(size, PMO_DATA_NOCACHE, MALLOC_TYPE_DEFAULT);
	if (dma_handle->pmo < 0) {
		return NULL;
	}

	void *res =
		chcore_auto_map_pmo(dma_handle->pmo, size, VM_READ | VM_WRITE);
	if (res == NULL) {
		// TODO: free pmo cap
		return NULL;
	}

	ret = usys_get_phys_addr(res, &dma_handle->paddr);
	if (ret != 0) {
		// TODO: free pmo cap
		return NULL;
	}
	dma_handle->vaddr = (u64)res;

	return res;
}

void chcore_free_dma_mem(struct chcore_dma_handle *dma_handle)
{
	if (dma_handle == NULL || dma_handle->pmo < 0
	    || dma_handle->vaddr == 0) {
		BUG("dma_handle is invalid");
	}

	usys_unmap_pmo(SELF_CAP, dma_handle->pmo, dma_handle->vaddr);
	// TODO: free pmo cap
}
