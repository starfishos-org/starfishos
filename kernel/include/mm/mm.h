#pragma once

#include <mm/buddy.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE BUDDY_PAGE_SIZE
#endif

#include <common/util.h>
#include <mm/vmspace.h>

/* Forward declarations */
struct vmspace;

#define N_PHYS_MEM_POOLS 8

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
/* Flush TLB only for CPUs belonging to a specific machine.
 * Returns 0 once that machine has acknowledged, -ETIMEDOUT otherwise. */
int flush_tlb_on_remote_machine(struct vmspace *vmspace, mid_t machine_id,
                                vaddr_t start_va, size_t len);
/* Drop this vmspace's translations on every other machine that has run it. */
void flush_tlbs_all_machines(struct vmspace *vmspace);
void memcpy_and_flush_tlb_on_remote_machine(struct vmspace *vmspace,
                                            mid_t machine_id, paddr_t src_pa,
                                            paddr_t dst_pa, size_t len,
                                            vaddr_t fault_va);
struct polling_tlb_batch_entry;
/*
 * Returns 0 when the whole batch was migrated. -EINPROGRESS transfers the
 * accepted transaction to a durable pending slot; any other non-zero return
 * means the remote machine applied nothing.
 */
int migrate_pages_to_shm_batch(mid_t target_mid, struct vmspace *vmspace,
                               struct pmobject *pmo, u64 first_index,
                               struct polling_tlb_batch_entry *entries,
                               u64 count, u64 perm);
int reap_pending_shm_migrations(void);
#endif
