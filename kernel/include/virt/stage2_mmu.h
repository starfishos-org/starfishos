#pragma once
#include <common/types.h>
#include <common/list.h>
#include <arch/mmu.h>
#include <arch/virt/stage2_page_table.h>

struct s2mmu {
	struct list_head ipa_region_list;
	ptp_t *pgtbl;
};

extern struct s2mmu boot_stage2_mmu;
struct s2mmu *create_stage2_mmu(void);
void destroy_stage2_mmu(struct s2mmu *s2mmu);
int s2mmu_map_range(struct s2mmu *s2mmu, paddr_t ipa, paddr_t pa,
		    int start_level, size_t len, int enable_huge_page,
		    vm_flags flags);
int s2mmu_query(struct s2mmu *s2mmu, paddr_t ipa, paddr_t *pa, int *level_out);
void install_stage2_pt(struct s2mmu *s2mmu, u64 vm_id);
void uninstall_stage2_pt(void);
int handle_guest_stage2_page_fault(struct s2mmu *s2mmu, u64 fault_reason,
				   int is_instruction_abort, int is_write_abort,
				   paddr_t fault_ipa, paddr_t fault_va);
void flush_stage2_tlb(void);

static inline void write_vttbr(paddr_t vttbr_value)
{
	asm volatile("msr vttbr_el2, %0" : : "r"(vttbr_value));
}
