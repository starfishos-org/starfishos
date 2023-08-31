#include <mm/mm.h>
#include <mm/vmspace.h>
#include <mm/kmalloc.h>
#include <common/kprint.h>
#include <common/macro.h>
#include <common/types.h>
#include <common/errno.h>
#include <irq/ipi.h>

#include <arch/mm/page_table.h>

/* Operations that invalidate TLBs and Paging-Structure Caches */

/*
 * flush_tlb(void *addr): flush tlb for one address
 *
 * invlpg:
 *       - flush corresponding tlb with current PCID
 *       - flush global tlb with the physical page number, regardless of PCID
 *       - flush all paging-structure cacahes with current PCID
 */
void flush_single_tlb(vaddr_t addr)
{
	asm volatile("invlpg (%0)" : : "r" (addr) : "memory");
}

/*
 * INVPCID: 4 types as follows:
 */
#define INVPCID_TYPE_INDIV_ADDR		0
#define INVPCID_TYPE_SINGLE_CTXT	1
#define INVPCID_TYPE_ALL_INCL_GLOBAL	2
#define INVPCID_TYPE_ALL_NON_GLOBAL	3

void __invpcid(u64 pcid, u64 addr, u64 type)
{
	struct { u64 d[2]; } __attribute__ ((aligned (16))) desc = { { pcid, addr } };

	/*
	 * The memory clobber is because the whole point is to invalidate
	 * stale TLB entries and, especially if we're flushing global
	 * mappings, we don't want the compiler to reorder any subsequent
	 * memory accesses before the TLB flush.
	 *
	 * The hex opcode is invpcid (%ecx), %eax in 32-bit mode and
	 * invpcid (%rcx), %rax in long mode.
	 */
	asm volatile (".byte 0x66, 0x0f, 0x38, 0x82, 0x01"
		      : : "m" (desc), "a" (type), "c" (&desc) : "memory");
}

/* Flush all mappings for a given pcid and addr, not including globals. */
// static inline
void invpcid_flush_one(u64 pcid, u64 addr)
{
	__invpcid(pcid, addr, INVPCID_TYPE_INDIV_ADDR);
}

/* Flush all mappings for a given PCID, not including globals. */
// static inline
void invpcid_flush_single_context(u64 pcid)
{
	__invpcid(pcid, 0, INVPCID_TYPE_SINGLE_CTXT);
}

/* Flush all mappings, including globals, for all PCIDs. */
// static inline
void invpcid_flush_all(void)
{
	__invpcid(0, 0, INVPCID_TYPE_ALL_INCL_GLOBAL);
}

/* Flush all mappings for all PCIDs except globals. */
// static inline
void invpcid_flush_all_nonglobals(void)
{
	__invpcid(0, 0, INVPCID_TYPE_ALL_NON_GLOBAL);
}

/*
 * x86_64 have several other options to flush all tlb.
 */

/*
 * MOV to CR3 when CR4.PCIDE = 1:
 *     - if bit 63 of the instruction source operand is 0: flush TLB with the PCID
 *     - if bit 63 is 1: do not flush TLB
 */

#ifdef CHCORE
void flush_tlb_all(void)
{
#if 1
	invpcid_flush_all();
#else
	/* If PCID is not supported, flush TLB with writing CR3 */
	paddr_t pgtbl;

	pgtbl = get_page_table();
	/* clear the highest bit: flush TLB of current PCID */
	pgtbl &= ~(1UL << 63);
	// printk("pgtbl: 0x%lx\n", pgtbl);
	set_page_table(pgtbl);
#endif
}
#endif

/*
 * IPI sender side:
 * Based on IPI_tx interfaces, ChCore uses the following TLB shootdown
 * protocol between different CPU cores.
 */
static void flush_remote_tlb_with_ipi(u32 target_cpu, vaddr_t start_va,
				      u64 page_cnt, u64 pcid, u64 vmspace)
{
	/* IPI_tx: step-1 */
	prepare_ipi_tx(target_cpu);

	/* IPI_tx: step-2 */
	/* set the first argument */
	set_ipi_tx_arg(target_cpu, 0, start_va);
	/* set the second argument */
	set_ipi_tx_arg(target_cpu, 1, page_cnt);
	/* set the third argument */
	set_ipi_tx_arg(target_cpu, 2, pcid);
	/* set the fourth argument */
	set_ipi_tx_arg(target_cpu, 3, vmspace);

	/* IPI_tx: step-3 */
	start_ipi_tx(target_cpu, IPI_TLB_SHOOTDOWN);

	/* IPI_tx: step-4 */
	wait_finish_ipi_tx(target_cpu);
}

/* Currently, ChCore uses a simple policy for choosing how to flush TLB */
// TODO: refer to Linux on how to flush TLB (for better performance)
#define TLB_SHOOTDOWN_THRESHOLD 2
void flush_local_tlb_opt(vaddr_t start_va, u64 page_cnt, u64 pcid)
{
	if (page_cnt > TLB_SHOOTDOWN_THRESHOLD) {
		/* Flush all the TLBs of the PCID */
		invpcid_flush_single_context(pcid);
	}
	else {
		u64 i;
		u64 addr;

		/* Flush each TLB entry one-by-one */
		addr = start_va;
		for (i = 0; i < page_cnt; ++i) {
			invpcid_flush_one(pcid, addr);
			addr += PAGE_SIZE;
		}
	}

}

/*
 * This function is responsible for flushing the TLBs (with the
 * corresponding VA range provided) on each necessary CPU.
 */
void flush_tlb_local_and_remote(struct vmspace* vmspace,
				vaddr_t start_va, size_t len)
{
	/* page_cnt, i.e., TLB_entry_cnt */
	u64 page_cnt;
	u64 pcid;
	u32 cpuid;
	u32 i;

	if (unlikely(len < PAGE_SIZE))
		kwarn("func: %s. len (%p) < PAGE_SIZE\n", __func__, len);

	if (len == 0)
		return;

	len = ROUND_UP(len, PAGE_SIZE);
	page_cnt = len / PAGE_SIZE;

	pcid = get_pcid(vmspace->pgtbl);

	/* Flush local TLBs */
	flush_local_tlb_opt(start_va, page_cnt, pcid);

	cpuid = smp_get_cpu_id();
	/* Flush remote TLBs */
	// TODO: it may be, sometimes, effective to interrupt all other CPU at the same time.
	for (i = 0; i < PLAT_CPU_NUM; ++i) {
		if ((i != cpuid) && (vmspace->history_cpus[i] == 1)) {
			flush_remote_tlb_with_ipi(i, start_va, page_cnt,
						  pcid, (u64)vmspace);
		}
	}
}

/*
 * This function is responsible for flushing the TLBs (with the
 * corresponding PCID provided) on each necessary CPU.
 */
static void flush_tlb_by_pcid_global(struct vmspace *vmspace)
{
	/* The dummy_va will not be used. */
	u64 dummy_va = 0;
	/* Set page_cnt to inifite for flushing all the TLBs of the PCID. */
	u64 page_cnt = -1;
	/* The dummy_vmspace will not be used. */
	u64 dummy_vmspace = 0;
	u64 pcid = vmspace->pcid;
	u32 cpuid;
	u32 i;

	/* Flush local TLBs */
	flush_local_tlb_opt(dummy_va, page_cnt, pcid);

	cpuid = smp_get_cpu_id();
	/* Flush remote TLBs */
	for (i = 0; i < PLAT_CPU_NUM; ++i) {
		if ((i != cpuid) && (vmspace->history_cpus[i] == 1)) {
			flush_remote_tlb_with_ipi(i, dummy_va, page_cnt,
						  pcid, dummy_vmspace);
		}
	}
}

void flush_tlb_of_vmspace(struct vmspace *vmspace)
{
	flush_tlb_by_pcid_global(vmspace);
}

void flush_tlbs(struct vmspace* vmspace, vaddr_t start_va, size_t len)
{
	flush_tlb_local_and_remote(vmspace, start_va, len);
}
