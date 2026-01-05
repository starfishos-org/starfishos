#pragma once

#include <common/util.h>
#include <mm/vmspace.h>
#include <mm/buddy.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE BUDDY_PAGE_SIZE
#endif

#define N_PHYS_MEM_POOLS 1

/* Execute once during kernel init. */
void mm_init(void *physmem_info, int clear_nvm);

void ext_mm_init(void);

/* remap mem range in kernel page table */
void remap_memory(u64 from_addr, u64 to_addr, u64 mem_size);

/* Return the size of free memory in the buddy and slab allocator. */
unsigned long get_free_mem_size(void);

/* Implementations differ on different architectures. */
void set_page_table(paddr_t pgtbl);
void flush_tlbs(struct vmspace *, vaddr_t start_va, size_t size);
#ifdef MULTI_PAGETABLE_ENABLED
/* Flush TLB only for CPUs belonging to a specific machine */
void flush_tlb_on_remote_machine(struct vmspace *vmspace, mid_t machine_id,
                                 vaddr_t start_va, size_t len);
void memcpy_and_flush_tlb_on_remote_machine(struct vmspace *vmspace,
                                            mid_t machine_id, paddr_t src_pa,
                                            paddr_t dst_pa, size_t len,
                                            vaddr_t fault_va);
paddr_t migrate_pages_to_shm(mid_t target_mid, struct vmspace *vmspace,
                              paddr_t src_pa, size_t len, vaddr_t fault_va);
#endif
