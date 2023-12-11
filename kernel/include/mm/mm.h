#pragma once

#include <common/util.h>
#include <mm/vmspace.h>
#include <mm/buddy.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE BUDDY_PAGE_SIZE
#endif

#define N_PHYS_MEM_POOLS 8

/* Execute once during kernel init. */
void mm_init(void *physmem_info, int clear_nvm);

void ext_mm_init(void);

/* Return the size of free memory in the buddy and slab allocator. */
unsigned long get_free_mem_size(void);

/* Implementations differ on different architectures. */
void set_page_table(paddr_t pgtbl);
void flush_tlbs(struct vmspace *, vaddr_t start_va, size_t size);
