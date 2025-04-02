#include <mm/vmspace.h>

void flush_cache_page(struct vmregion *vmr, unsigned long vaddr, unsigned long pfn);

void flush_cache_range(struct vmregion *vmr, unsigned long vaddr, unsigned long len);