#include <common/macro.h>
#include <common/util.h>
#include <common/list.h>
#include <common/errno.h>
#include <common/kprint.h>
#include <common/types.h>
#include <common/lock.h>
#include <lib/printk.h>
#include <mm/vmspace.h>
#include <mm/kmalloc.h>
#include <mm/buddy.h>
#include <mm/mm.h>
#include <mm/nvm.h>
#include <mm/vmspace.h>
#include <mm/nvm.h>
#include <mm/rmap.h>
#include <arch/mmu.h>
#include <object/thread.h>
#include <object/cap_group.h>
#include <object/user_fault.h>
#include <sched/context.h>
#include <arch/sync.h>
#include <arch/mm/page_table.h>
#include <ckpt/hot_pages_tracker.h>
#include <ckpt/ckpt.h>

extern void pagecpy(void *dst, const void *src);
extern void pagecpy_nt(void *dst, const void *src);

/* Policy on-demand: only mapping the faulting address */
#define ONDEMAND  0

/*
 * Not implemented now.
 * Policy pre-fault: mapping the serveral continous pages in advance.
 */
#define PREFAULT  1

#define PGFAULT_POLICY ONDEMAND

#if PGFAULT_POLICY == ONDEMAND

int pa_checksum(const char *page) {
	int i, sum = 0;
	for (i = 0; i < PAGE_SIZE; ++i)
		sum += page[i];
	return sum;
}

int pas_checksum(const char *page, int size) {
	int i, sum = 0;
	for (i = 0; i < size; ++i)
		sum += page[i];
	return sum;
}

static int size_to_page_order(unsigned long size)
{
	unsigned long order;
	unsigned long pg_num;
	unsigned long tmp;

	order = 0;
	pg_num = ROUND_UP(size, BUDDY_PAGE_SIZE) / BUDDY_PAGE_SIZE;
	tmp = pg_num;

	while (tmp > 1) {
		tmp >>= 1;
		order += 1;
	}

	if (pg_num > (1 << order))
		order += 1;

	return (int)order;
}

extern u64 tmp;

/* add_pte_patch_to_pool: when trigger write, track pages's pte and page struct
 */
void add_pte_patch_to_pool(struct vmspace *vmspace, pte_t *pte, struct page *page)
{
	struct pte_patch_pool *pool, *new_pool;
	
	pool = (struct pte_patch_pool *)vmspace->pte_patch_pool;
	if (pool) {
		/* set next pte entry */
		/* check if vmspace lock is already get? */
		// lock(&vmspace->pte_patch_lock);
		pool->array[pool->count] = (struct pte_patch_pool_entry){
								.pte = pte, .page = page};
		// printk("add_pte_patch_to_pool: page=%p\n", page);
		if (++pool->count >= MAX_ENTRY) {
			/* Full, alloc a new pool */
			new_pool = (struct pte_patch_pool *)create_patch_pool();
			new_pool->next = pool;
			vmspace->pte_patch_pool = new_pool;
		}
		// unlock(&vmspace->pte_patch_lock);
	} else {
		// kinfo("[ERR] empty pte_patch_pool\n");
	}
}

#ifdef OMIT_BENCHMARK
static inline bool need_omit(struct vmspace *vmspace)
{
	return vmspace == redis_benchmark_vmspace
			|| vmspace == memcachetest_vmspace || vmspace == ycsb_vmspace;
}
#endif
#ifdef REPORT
u64 patch_page_num = 0;
#endif
#ifdef REPORT_RUNTIME
extern u64 pf_count;
extern u64 pf_tot_time;
#endif
int map_page_in_pgtbl(struct vmspace *vmspace, vaddr_t va, paddr_t pa,
	vmr_prop_t flags, pte_t **out_pte);
int handle_trans_fault(struct vmspace *vmspace, vaddr_t fault_addr, int present, int write)
{
#ifdef REPORT_RUNTIME
	DECLTMR;start();
#endif
	struct vmregion *vmr;
	struct pmobject *pmo;
	struct page *page;
	paddr_t pa;
	u64 offset;
	u64 index;
	int ret = 0;
	pte_t *pte; /* out pte */
	int ckpt_ret = 0;
#if defined(OMIT_PF) && !defined(PMO_CHECKSUM)
	UNUSED(page);
#endif
	/*
	 * Grab lock here.
	 * Because two threads (in same process) on different cores
	 * may fault on the same page, so we need to prevent them
	 * from adding the same mapping twice.
	 */
	lock(&vmspace->vmspace_lock);
	vmr = find_vmr_for_va(vmspace, fault_addr);
	if (vmr == NULL) {
		kinfo("[dbg] PF: P %d W %d\n", present, write);
		kinfo("handle_trans_fault: no vmr found for va 0x%lx!\n",
		      fault_addr);
		kinfo("process: %p\n", current_cap_group);
		print_thread(current_thread);
		kinfo("faulting IP: 0x%lx, SP: 0x%lx\n",
		      arch_get_thread_next_ip(current_thread),
		      arch_get_thread_stack(current_thread));
		extern u64 tmp;
		kinfo("fault_ins_addr: 0x%lx\n",tmp);
		kprint_vmr(vmspace);
		// TODO: kill the process
		kwarn("TODO: kill such faulting process.\n");
		return -ENOMAPPING;
	}
	if (present && !(vmspace->flags & VM_FLAG_PRESERVE) && !(vmr->perm & VMR_COW)) {
			printk("perm %u, pmo_flags %u, write %u, pmo_type %u\n",
					vmr->perm,
					vmspace->flags & VM_FLAG_PRESERVE,
					write,
					vmr->pmo->type);
			/* The PTE is valid, it's a permission error */
            kinfo("General Protection Fault\n");
		if (write) {
			kinfo("Cannot write at %p.\n", fault_addr);
		} else {
			kinfo("Cannot read at %p.\n", fault_addr);
		}
		while(1);
	}
	
	pmo = vmr->pmo;
	switch (pmo->type) {
	case PMO_DATA:
	case PMO_FILE:
	case PMO_RING_BUFFER:
	case PMO_RING_BUFFER_RADIX:
	case PMO_ANONYM:
	case PMO_SHM: {

		BUG_ON(pmo->type == PMO_RING_BUFFER && present);

		vmr_prop_t perm;

		perm = vmr->perm;

		/* Get the offset in the pmo for faulting addr */
		offset = ROUND_DOWN(fault_addr, PAGE_SIZE) - vmr->start;
		/* Boundary check */
		if ((offset >= pmo->size) && (pmo->type == PMO_FILE)) {
			static int warn = 1;

			if (warn == 1) {
				kwarn("%s (out-of-range writing) offset 0x%lx, "
				      "pmo->size 0x%lx, FILE\n",
				      __func__, offset, pmo->size);
				warn = 0;
			}

			/*
			 * FIXME: we simply allow it now by adding new pages.
			 * For PMO_FILE, users can mmap a memory that is larger
			 * than the file size. If they accesses bytes beyond
			 * file size, SIGBUS should be triggered on LInux.
			 *
			 * TODO: why setting all the perm here?
			 */
			perm = VMR_READ | VMR_WRITE | VMR_EXEC;
		} else {
			BUG_ON(offset >= pmo->size);
		}

		/* Get the index in the pmo radix for faulting addr */
		index = offset / PAGE_SIZE;

		fault_addr = ROUND_DOWN(fault_addr, PAGE_SIZE);

		pa = get_page_from_pmo(pmo, index);
		// printk("[debug] %s: paddr=%llx\n", __func__, pa);

		/* PMO_FILE fault means user fault */
		if (pmo->type == PMO_FILE && !pa) {
			/* pa != 0 means this fault is cause by ckpt/restore */
#ifdef CHCORE_ENABLE_FMAP
			unlock(&vmspace->vmspace_lock);
			handle_user_fault(pmo, fault_addr);
			/* One short-cut exit */
			BUG("Should never be here!\n");
#else
			ret = -EINVAL;
			break;
#endif
		}

		if (pa == 0) {
			/* Not committed before. Then, allocate the physical page. */
			void *new_va = get_pages(0);
			BUG_ON(new_va == NULL);
			pa = virt_to_phys(new_va);
			BUG_ON(pa == 0);
			/* Clear to 0 for the newly allocated page */
			memset(new_va, 0, PAGE_SIZE);
			/*
			 * Record the physical page in the radix tree:
			 * the offset is used as index in the radix tree
			 */
			kdebug("commit: index: %ld, 0x%lx\n", index, pa);
			commit_page_to_pmo(pmo, index, pa);

			/* Add mapping in the page table */
			lock(&vmspace->pgtbl_lock);
			map_page_in_pgtbl(vmspace->pgtbl, fault_addr, pa,
						perm, &pte);
			unlock(&vmspace->pgtbl_lock);

			/* do not persist pages belong to external sync pmo */
			if (is_external_sync_pmo(pmo))
				break;
#ifdef OMIT_BENCHMARK
			/* omit track page fault of redis-benchmark */
			if (need_omit(vmspace)) {
				break;
			}
#endif
#ifndef OMIT_PF
			page = virt_to_page(new_va);
			if ((vmspace->flags & VM_FLAG_PRESERVE) && !is_external_sync_pmo(pmo)) {
			#ifdef PMO_CHECKSUM
				page->ckpt_version_number = get_current_ckpt_version() + 1;
			#endif
				// int ckpt_ret = 
				#ifndef OMIT_BENCHMARK
				ckpt_nvm_page(pmo, new_va, index);
				#endif
				add_pte_patch_to_pool(vmspace, (pte_t *)pte, page);
				// if(ckpt_ret) {
				// 	track_access(page);
				// }
			}
#endif
		}
		else if (vmr->perm & VMR_COW) {
			/* write to a COWed page */
			perm &= (~VMR_COW);
			if (pmo->type == PMO_FILE) {
				perm |= VMR_EXEC;
			}
			// BUG_ON(write == 0);
			struct page *p_page;
			if (pmo->type == PMO_DATA) {
				p_page = virt_to_page((void*)phys_to_virt(pmo->start));
				size_t size = pmo->size;
				lock(&p_page->lock);
				if (p_page->ref_cnt > 1) {
					void *new_page = get_pages(size_to_page_order(size));
					BUG_ON(new_page == 0);
					memcpy(new_page, (void*)phys_to_virt(pmo->start), size);
					p_page->ref_cnt--;
					pmo->start = virt_to_phys(new_page);
				}
				unlock(&p_page->lock);
				lock(&vmspace->pgtbl_lock);
				map_range_in_pgtbl(vmspace->pgtbl, vmr->start, pmo->start,
						size, perm, (u64 **)&pte);		
				unlock(&vmspace->pgtbl_lock);
			}
			else {
				BUG_ON(/*pmo->type == PMO_FILE ||*/ pmo->type == PMO_SHM);
				if (!write) {
					perm |= VMR_COW;
					lock(&vmspace->pgtbl_lock);
					map_range_in_pgtbl(vmspace, fault_addr, pa,
						PAGE_SIZE, perm, (u64 **)&pte);		
					unlock(&vmspace->pgtbl_lock);
				} else {
					p_page = virt_to_page((void*)phys_to_virt(pa));
					lock(&p_page->lock);
					if (p_page->ref_cnt > 1) {
						void *new_page = get_pages(0);
						BUG_ON(new_page == 0);
						memcpy(new_page, (void*)phys_to_virt(pa), PAGE_SIZE);
						p_page->ref_cnt--;
						/* new pa */
						pa = virt_to_phys(new_page);
						commit_page_to_pmo(pmo, index, pa);
					} else {
						BUG_ON(p_page->ref_cnt != 1);
					}
					unlock(&p_page->lock);
					lock(&vmspace->pgtbl_lock);
					map_range_in_pgtbl(vmspace, fault_addr, pa,
							PAGE_SIZE, perm, (u64 **)&pte);		
					unlock(&vmspace->pgtbl_lock);
				}				
			}
		}
		else {
			/*
			 * pa != 0: the faulting address has be committed a
			 * physical page.
			 *
			 * For concurrent page faults:
			 *
			 * When type is PMO_ANONYM, the later faulting threads
			 * of the process do not need to modify the page
			 * table because a previous faulting thread will do
			 * that. (This is always true for the same process)
			 * However, if one process map an anonymous pmo for
			 * another process (e.g., main stack pmo), the faulting
			 * thread (e.g, in the new process) needs to update its
			 * page table.
			 * So, for simplicity, we just update the page table.
			 * Note that adding the same mapping is harmless.
			 *
			 * When type is PMO_SHM, the later faulting threads
			 * needs to add the mapping in the page table.
			 * Repeated mapping operations are harmless.
			 */

			if (pmo->type == PMO_FILE) {
				perm |= VMR_EXEC;
			}
#ifndef OMIT_PF
			if ((vmspace->flags & VM_FLAG_PRESERVE) && !write && !is_external_sync_pmo(pmo)) {
				/* Read preserved page, map as read-only */
				perm &= ~VMR_WRITE;
			}
#endif
			/* Add mapping in the page table */
			pte_t *pte = NULL;
			lock(&vmspace->pgtbl_lock);
			map_page_in_pgtbl(vmspace->pgtbl, fault_addr, pa, perm, &pte);
			unlock(&vmspace->pgtbl_lock);

			/* do not persist pages belong to external sync pmo */
			if (is_external_sync_pmo(pmo))
				break;
#ifndef OMIT_PF
#ifdef OMIT_BENCHMARK
			/* omit track page fault of benchmarks */
			if (need_omit(vmspace)) {
				break;
			}
#endif
			page = virt_to_page((void*)phys_to_virt(pa));		
			if (write && (vmspace->flags & VM_FLAG_PRESERVE)) {
				BUG_ON(unlikely(!page));
				if (unlikely(get_page_type(page) != NVM_PAGE)) {
					BUG("page(%p) is not NVM page, type=%d, flag=%d, pte=0x%llx, pmo_type=%d, %llx\n", 
						page, get_page_type(page), page->flags, pte, pmo->type, pmo->dram_cache.array);
				}
#ifndef OMIT_MEMCPY
				/* copy page to ckpt_page */
				if (pmo != page->pmo) {
					page->pmo = pmo;
				}
				ckpt_ret = ckpt_nvm_page(pmo, (void *)phys_to_virt(pa), index);
#endif
				/* Add pte patch */
				add_pte_patch_to_pool(vmspace, pte, page);
#ifdef HYBRID_MEM
				if(!ckpt_ret)
					track_access(page);
#else
				UNUSED(ckpt_ret);
#endif
			#ifdef REPORT_RUNTIME
				pf_count++;
				pf_tot_time += stop();
				LOG("[ckpt=%llu] [page fault count] page=%p, pte=%p\n", CKPT_VERSION_NUMBER, page, pte);
			#endif			
			}
#else
	UNUSED(ckpt_ret);
#endif		
		}
		break;
	}
	case PMO_FORBID: {
		kinfo("Forbidden memory access (pmo->type is PMO_FORBID).\n");
		BUG_ON(1);
		break;
	}
	default: {
		kinfo("handle_trans_fault: faulting vmr->pmo->type"
		      "(pmo type %d at 0x%lx)\n", vmr->pmo->type, fault_addr);
		kinfo("Currently, this pmo type should not trigger pgfaults\n");
		kprint_vmr(vmspace);
		BUG_ON(1);
		return -ENOMAPPING;
	}
	}

	unlock(&vmspace->vmspace_lock);
	return ret;
}

#endif
