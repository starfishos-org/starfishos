#include <arch/mm/cache.h>

#include <arch/mm/page_table.h>

// for kernel space, paddr is same with virt_to_paddr(vaddr)
static void __flush_dcache_pages(paddr_t paddr, unsigned long vaddr, unsigned nr)
{
	__asm__ __volatile__("clflush %0" : : "m"(paddr));
}

static void __inv_icache_pages(paddr_t paddr, unsigned long vaddr, unsigned nr)
{
    __asm__ __volatile__("invlpg %0" : : "m"(paddr));
}

void flush_cache_page(struct vmregion *vmr, unsigned long vaddr, unsigned long pfn)
{
    paddr_t paddr = pfn << PAGE_SHIFT;
    __flush_dcache_pages(paddr, vaddr, 1);
    if (vmr->perm & VMR_EXEC) {
        __inv_icache_pages(paddr, vaddr, 1);
    }
}

void flush_cache_range(struct vmregion *vmr, unsigned long vaddr, unsigned long len)
{
    __flush_dcache_pages(vaddr, vaddr, len / PAGE_SIZE);
    if (vmr->perm & VMR_EXEC) {
        __inv_icache_pages(vaddr, vaddr, len / PAGE_SIZE);
    }
}
