#pragma once

#include <mm/vmspace.h>

void flush_tlb_all(void);
void flush_tlb_of_vmspace(struct vmspace *vmspace);
void flush_tlb_local_and_remote(struct vmspace *vmspace, vaddr_t start_va,
                                size_t len);
void flush_tlbs(struct vmspace *vmspace, vaddr_t start_va, size_t len);
