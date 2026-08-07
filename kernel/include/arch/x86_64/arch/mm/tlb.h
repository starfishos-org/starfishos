#pragma once

#include <mm/vmspace.h>

struct tlb_flush_batch_op {
    u64 fault_va;
    u64 len;
    u64 pcid;
    u64 vmspace_ptr;
};

void flush_tlb_all(void);
void flush_tlb_of_vmspace(struct vmspace *vmspace);
/* Clear a (possibly recycled) PCID on every CPU before its first use. */
void flush_tlb_by_pcid_all_cpus(u64 pcid);
void flush_tlb_local_and_remote(struct vmspace *vmspace, vaddr_t start_va,
                                size_t len);
void flush_tlbs(struct vmspace *vmspace, vaddr_t start_va, size_t len);
void flush_tlb_batch_local(struct tlb_flush_batch_op *ops, u64 ops_count);
void flush_tlbs_batch_on_all_cpus(struct tlb_flush_batch_op *ops,
                                  u64 ops_count);
