#include <common/util.h>
#include <common/vars.h>
#include <common/types.h>
#include <common/kprint.h>
#include <common/macro.h>
#include <mm/kmalloc.h>
#include <mm/vmspace.h>
#include <mm/mm.h>
#include <mm/buddy.h>
#include <mm/rmap.h>
#include <object/thread.h>
#include <object/object.h>
#include <object/cap_group.h>
#include <ckpt/hot_pages_tracker.h>
#include <ckpt/ckpt.h>

/* Another way to access user memory is disabling SMAP and
 * directly access them.
 */

extern int map_page_in_pgtbl(void *pgtbl, vaddr_t va, paddr_t pa,
                             vmr_prop_t flags, pte_t **out_pte);
extern int query_in_pgtbl(void *pgtbl, vaddr_t va, paddr_t *pa, pte_t **entry);
extern void set_pte_dirty(pte_t *entry);

#define PAGE_OFFSET_MASK 0xFFF
vaddr_t transform_vaddr(char *user_buf, bool write)
{
        struct vmspace *vmspace;
        struct vmregion *vmr;
        struct pmobject *pmo;
        vaddr_t kva;

        u64 offset, index;
        vaddr_t va; /* Aligned va of user_buf */
        paddr_t pa;

        vmspace = obj_get(current_cap_group, VMSPACE_OBJ_ID, TYPE_VMSPACE);

        /* Prevent concurrent operations on the vmspace */
        write_lock(&vmspace->vmspace_lock);

        vmr = find_vmr_for_va(vmspace, (vaddr_t)user_buf);
        /* Target user address is not valid (not mapped before) */
        BUG_ON(vmr == NULL);
        pmo = vmr->pmo;
        BUG_ON(pmo == NULL);

        va = ROUND_DOWN((u64)user_buf, PAGE_SIZE);
        offset = va - vmr->start;
        index = offset / PAGE_SIZE;

        /*
         * A special case: the address is mapped with an anonymous pmo
         * which is not directly mapped by the user.
         * And, kernel touches it first.
         *
         * To prevent pgfault, kernel maps the page first if necessary.
         *
         * pmo->type is data: must be mapped
         */
        switch (pmo->type) {
        case PMO_DATA:
        case PMO_DATA_NOCACHE:
#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
        case PMO_RING_BUFFER: {
                /*
                 * Calculate the kva for the user_buf:
                 * kva = start_addr + offset
                 */
                // pa = vmr->pmo->start + (paddr_t)(user_buf - vmr->start);
                pa = get_page_from_pmo(pmo, offset / PAGE_SIZE);
                kva = phys_to_virt(pa);
                break;
        }
#endif /* CHCORE_SLS */
        case PMO_ANONYM: {
                /* Boundary check */
                BUG_ON(offset >= pmo->size);
                /* Get the physical page for va according to the radix tree in
                 * the pmo */
                pa = get_page_from_pmo(pmo, index);

                if (pa == 0) {
                        /* No physical page allocated to it before */
                        // void *new_va = get_dram_pages(0);
                        void *new_va = get_pages(0, __DEFAULT__);
                        BUG_ON(new_va == NULL);
                        pa = virt_to_phys(new_va);
                        BUG_ON(pa == 0);
                        memset(new_va, 0, PAGE_SIZE);
#ifdef RMAP_ENABLED
                        commit_page_to_pmo(pmo, index, pa);
#endif
                        pte_t *pte;
                        lock(&vmspace->pgtbl_lock);
                        map_page_in_pgtbl(
                                vmspace->pgtbl, va, pa, vmr->perm, &pte);
                        unlock(&vmspace->pgtbl_lock);
#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
#ifndef OMIT_PF
                        if ((vmspace->flags & VM_FLAG_PRESERVE)
                            && !is_external_sync_pmo(pmo)) {
                                struct page *page = virt_to_page(new_va);
                                BUG_ON(page->pmo != pmo);
                                // int ckpt_ret =
#ifndef OMIT_BENCHMARK
                                #ifdef CHCORE_SSI_SLS
                                ckpt_dsm_page(pmo, new_va, index);
                                #else
                                ckpt_nvm_page(pmo, new_va, index);
                                #endif
                                
#endif
                                add_pte_patch_to_pool(vmspace, pte, page);
                                // if(ckpt_ret) {
                                // 	track_access(page);
                                // }
                                // track_access(page);
                        }
#endif
#endif /* CHCORE_SLS */
                }
                /*
                 * Return: start address of the newly allocated page +
                 *         offset in the page (last 12 bits).
                 */
                kva = phys_to_virt(pa)
                      + (((vaddr_t)user_buf) & PAGE_OFFSET_MASK);
                break;
        }
#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
        case PMO_RING_BUFFER_RADIX:
#endif /* CHCORE_SLS */
        case PMO_SHM:
#ifdef USE_CXL_MEM
        case PMO_CROSS_SHM:
#endif
        case PMO_FILE: {
                /*
                 * Boundary check.
                 *
                 * No boundary check for PMO_FILE because a file can
                 * be mapped to a larger (larger than the file size) vmr.
                 * (allowed in Linux)
                 *
                 * TODO: make the behaviour more accurate in ChCore
                 */
                if (pmo->type == PMO_SHM || pmo->type == PMO_RING_BUFFER_RADIX)
                        BUG_ON(offset >= pmo->size);

#ifdef USE_CXL_MEM
 if (pmo->type == PMO_CROSS_SHM)
                        BUG_ON(offset >= pmo->size);
      #endif
                /* Get the physical page for va according to the radix tree in
                 * the pmo */
                pa = get_page_from_pmo(pmo, index);

                if (pa == 0) {
                        /*
                         * No physical page allocated to it before.
                         * SHM should not be accessed by kernel first.
                         */
                        kwarn("kernel accessing PMO_SHM before user\n");
                        BUG_ON(1);
                }
                /*
                 * Return: start address of the newly allocated page +
                 *         offset in the page (last 12 bits).
                 */
                kva = phys_to_virt(pa)
                      + (((vaddr_t)user_buf) & PAGE_OFFSET_MASK);
                break;
        }
        default: {
                kinfo("bug: kernel accessing pmo type: %d\n", pmo->type);
                BUG_ON(1);
                break;
        }
        }
#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
        struct page *page = virt_to_page((void *)phys_to_virt(pa));
        if (write && (vmspace->flags & VM_FLAG_PRESERVE)
            && !is_external_sync_pmo(pmo)) {
                page_type_t page_type;
                pte_t *pte;
                page_type = get_page_type(page);
                switch (page_type) {
                case DRAM_CACHED_PAGE: {
                        BUG_ON(query_in_pgtbl(vmspace->pgtbl, va, NULL, &pte));
                        set_pte_dirty(pte);
                        break;
                }
                case NVM_PAGE: {
#ifndef OMIT_MEMCPY
                        #ifdef CHCORE_SLS
                        ckpt_nvm_page(pmo, (void *)phys_to_virt(pa), index);
                        #endif
#endif
                        break;
                }
                case CXL_MEM_PAGE: {
#ifndef OMIT_MEMCPY
                        #ifdef CHCORE_SSI_SLS
                        ckpt_dsm_page(pmo, (void *)phys_to_virt(pa), index);
                        #endif
#endif
                        break;
                }
                default: {
                        BUG_ON(1);
                }
                }
        }
#endif /* CHCORE_SLS */
        write_unlock(&vmspace->vmspace_lock);

        obj_put(vmspace);
        return kva;
}

#ifndef FBINFER

int copy_from_user(char *kernel_buf, char *user_buf, size_t size)
{
        vaddr_t kva;
        u64 start_off;
        u64 len;
        u64 left_size;

        len = 0;

        /* Validate user_buf */
        BUG_ON(((u64)user_buf >= KBASE) || ((u64)user_buf + size >= KBASE));

        /* For the frist user memory page */
        kva = transform_vaddr(user_buf, false);
        start_off = ((vaddr_t)user_buf) & PAGE_OFFSET_MASK;
        /* No more than one page */
        if (size + start_off <= PAGE_SIZE) {
                memcpy((void *)kernel_buf, (void *)kva, size);
                return 0;
        } else {
                len = PAGE_SIZE - start_off;
                memcpy((void *)kernel_buf, (void *)kva, len);
                user_buf += len;
                kernel_buf += len;
        }

        /* Intermediate memory pages */
        BUG_ON(((u64)user_buf % PAGE_SIZE) != 0);
        left_size = size - len;
        while (left_size > PAGE_SIZE) {
                kva = transform_vaddr(user_buf, false);
                memcpy((void *)kernel_buf, (void *)kva, PAGE_SIZE);
                user_buf += PAGE_SIZE;
                kernel_buf += PAGE_SIZE;
                left_size -= PAGE_SIZE;
        }

        /* The last memory page */
        kva = transform_vaddr(user_buf, false);
        memcpy((void *)kernel_buf, (void *)kva, left_size);

        return 0;
}

int copy_to_user(char *user_buf, char *kernel_buf, size_t size)
{
        vaddr_t kva;
        u64 start_off;
        u64 len;
        u64 left_size;

        len = 0;

        /* Validate user_buf */
        BUG_ON(((u64)user_buf >= KBASE) || ((u64)user_buf + size >= KBASE));

        /* For the frist user memory page */
        kva = transform_vaddr(user_buf, true);
        start_off = ((vaddr_t)user_buf) & PAGE_OFFSET_MASK;
        /* No more than one page */
        if (size + start_off <= PAGE_SIZE) {
                memcpy((void *)kva, (void *)kernel_buf, size);
                return 0;
        } else {
                len = PAGE_SIZE - start_off;
                memcpy((void *)kva, (void *)kernel_buf, len);
                user_buf += len;
                kernel_buf += len;
        }

        /* Intermediate memory pages */
        BUG_ON(((u64)user_buf % PAGE_SIZE) != 0);
        left_size = size - len;
        while (left_size > PAGE_SIZE) {
                kva = transform_vaddr(user_buf, true);
                memcpy((void *)kva, (void *)kernel_buf, PAGE_SIZE);
                user_buf += PAGE_SIZE;
                kernel_buf += PAGE_SIZE;
                left_size -= PAGE_SIZE;
        }

        /* The last memory page */
        kva = transform_vaddr(user_buf, true);
        memcpy((void *)kva, (void *)kernel_buf, left_size);

        return 0;
}

#endif
