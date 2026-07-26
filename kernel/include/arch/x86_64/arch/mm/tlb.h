#pragma once

#include <mm/vmspace.h>

void flush_tlb_all(void);
void flush_tlb_of_vmspace(struct vmspace *vmspace);
/* Clear a (possibly recycled) PCID on every CPU before its first use. */
void flush_tlb_by_pcid_all_cpus(u64 pcid);
void flush_tlb_local_and_remote(struct vmspace *vmspace, vaddr_t start_va,
                                size_t len);
void flush_tlbs(struct vmspace *vmspace, vaddr_t start_va, size_t len);
