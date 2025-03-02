#include <common/macro.h>
#include <common/util.h>
#include <common/list.h>
#include <common/errno.h>
#include <common/lock.h>
#include <common/kprint.h>
#include <mm/vmspace.h>
#include <mm/kmalloc.h>
#include <mm/mm.h>
#include <mm/buddy.h>
#include <mm/rmap.h>
#include <arch/mmu.h>
#include <object/thread.h>
#include <perf/measure.h>
#include <ckpt/hot_pages_tracker.h>
/* Local functions */

#if 0
static inline struct virtual_vmregion *alloc_virt_vmregion(struct vmregion *vmr)
{
	struct virtual_vmregion *virt_vmr;

	virt_vmr = kmalloc(sizeof(*virt_vmr), __DEFAULT__);
	lock_init(&(virt_vmr->lock));
	virt_vmr->vmr = vmr;

	return virt_vmr;
}

static void remove_vmr_from_virt_vmregion(struct vmregion *vmr) 
{
        struct virtual_vmregion *virt_vmr = vmr->virtual_vmr;
        lock(&virt_vmr->lock);
        BUG_ON(virt_vmr->vmr != vmr);
        virt_vmr->vmr = NULL;
        unlock(&virt_vmr->lock);
}
#endif

#ifdef OMIT_BENCHMARK
struct vmspace *redis_benchmark_vmspace = NULL;
struct vmspace *memcachetest_vmspace = NULL;
struct vmspace *ycsb_vmspace = NULL;
#endif

struct vmregion *alloc_vmregion(void)
{
    struct vmregion *vmr;

    vmr = kmalloc(sizeof(*vmr), __DEFAULT__);

    return vmr;
}

void free_vmregion(struct vmregion *vmr)
{
    kfree((void *)vmr);
}

/*
 * Return value:
 * -1: node1 (vm range1) < node2 (vm range2)
 * 0: overlap
 * 1: node1 > node2
 */
static bool cmp_two_vmrs(const struct rb_node *node1,
                         const struct rb_node *node2)
{
    struct vmregion *vmr1, *vmr2;
    vaddr_t vmr1_start, vmr1_end, vmr2_start;

    vmr1 = rb_entry(node1, struct vmregion, tree_node);
    vmr2 = rb_entry(node2, struct vmregion, tree_node);

    vmr1_start = vmr1->start;
    vmr1_end = vmr1_start + (vmr1->size > 0 ? (vmr1->size - 1) : vmr1->size);

    vmr2_start = vmr2->start;

    /* vmr1 < vmr2 */
    if (vmr1_end < vmr2_start)
        return true;

    /* vmr1 > vmr2 or vmr1 and vmr2 overlap */
    return false;
}

struct va_range {
    vaddr_t start;
    vaddr_t end;
};

/*
 * Return value:
 * -1: va_range < node (vmr)
 *  0: overlap
 *  1: va_range > node
 */
static int cmp_vmr_and_range(const void *va_range, const struct rb_node *node)
{
    struct vmregion *vmr;
    vaddr_t vmr_start, vmr_end;

    vmr = rb_entry(node, struct vmregion, tree_node);
    vmr_start = vmr->start;
    vmr_end = vmr_start + (vmr->size > 0 ? (vmr->size - 1) : vmr->size);

    struct va_range *range = (struct va_range *)va_range;
    /* range < vmr */
    if (range->end < vmr_start)
        return -1;

    /* range > vmr */
    if (range->start > vmr_end)
        return 1;

    /* range and vmr overlap */
    return 0;
}

/*
 * Return value:
 * -1: va < node (vmr)
 *  0: va belongs to node
 *  1: va > node
 */
static int cmp_vmr_and_va(const void *va, const struct rb_node *node)
{
    struct vmregion *vmr;
    vaddr_t vmr_start, vmr_end;

    vmr = rb_entry(node, struct vmregion, tree_node);
    vmr_start = vmr->start;
    vmr_end = vmr_start + (vmr->size > 0 ? (vmr->size - 1) : vmr->size);

    if ((vaddr_t)va < vmr_start)
        return -1;

    if ((vaddr_t)va > vmr_end)
        return 1;

    return 0;
}

/* Returns 0 when no intersection detected. */
static int check_vmr_intersect(struct vmspace *vmspace,
                               struct vmregion *vmr_to_add)
{
    struct va_range range;

    range.start = vmr_to_add->start;
    range.end =
            range.start + (vmr_to_add->size > 0 ? (vmr_to_add->size - 1) : 0);

    struct rb_node *res;
    res = rb_search(
            &vmspace->vmr_tree, (const void *)&range, cmp_vmr_and_range);
    /*
     * If rb_search returns NULL,
     * the vmr_to_add will not overlap with any existing vmr.
     */
    return (res == NULL) ? 0 : 1;
}

// find a free va for vmr to allocate
// make sure that the lock is acquired before calling this function
static vaddr_t find_free_va(struct vmspace *vmspace, size_t size)
{
    struct vmregion *vmr = NULL;
    vaddr_t va_last_high = 0, va_low = 0;

    // [xx, va_last_high) -> [find) -> [va_low, xx)
    for_each_in_list(vmr, struct vmregion, list_node, &vmspace->vmr_list) {
        va_low = vmr->start;
        if (va_low > va_last_high + size) {
            return va_last_high;
        }
        va_last_high = va_low + vmr->size;
    }
    return va_last_high;
}

struct vmspace *get_current_vmspace()
{
    return obj_get(current_thread->cap_group, VMSPACE_OBJ_ID, TYPE_VMSPACE);
}

int add_vmr_to_vmspace(struct vmspace *vmspace, struct vmregion *vmr)
{
    if (vmr->start == -1) {
        vmr->start = find_free_va(vmspace, vmr->size);
    }

    if (check_vmr_intersect(vmspace, vmr) != 0) {
        kwarn("VM fault: vmr overlap, vmr->va=%llx, ->size=%llx\n",
              vmr->start,
              vmr->size);
        /* TODO: kill the faulting process */
        // BUG_ON(1);
        return -EINVAL;
    }

    list_add(&vmr->list_node, &vmspace->vmr_list);
    rb_insert(&vmspace->vmr_tree, &vmr->tree_node, cmp_two_vmrs);
    vmr->vmspace = (void *)vmspace;
    return 0;
}

static int remove_vmr_from_vmspace(struct vmspace *vmspace,
                                   struct vmregion *vmr)
{
    if (check_vmr_intersect(vmspace, vmr) != 0) {
        rb_erase(&vmspace->vmr_tree, &vmr->tree_node);
        list_del(&vmr->list_node);
        vmr->vmspace = NULL;
        return 0;
    } else {
        return -1;
    }
}

static void del_vmr_from_vmspace(struct vmspace *vmspace, struct vmregion *vmr)
{
#ifdef RMAP_ENABLED
    pmo_remove_reverse_node(vmr->pmo, vmr);
#endif
    remove_vmr_from_vmspace(vmspace, vmr);
    free_vmregion(vmr);
}

static int fill_page_table(struct vmspace *vmspace, struct vmregion *vmr)
{
    size_t pm_size;
    paddr_t pa;
    vaddr_t va;
    vmr_prop_t perm;
    int ret;

    pm_size = vmr->pmo->size;
    pa = vmr->pmo->start;
    va = vmr->start;
    perm = vmr->perm;
#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
#ifndef OMIT_PF
    if ((vmspace->flags & VM_FLAG_PRESERVE) && !is_external_sync_pmo(vmr->pmo))
        perm &= ~(VMR_WRITE);
#endif
#endif
    lock(&vmspace->pgtbl_lock);
    ret = map_range_in_pgtbl(vmspace->pgtbl, va, pa, pm_size, perm);
    unlock(&vmspace->pgtbl_lock);

    return ret;
}

/* Dumping all the vmrs of one vmspace. */
void kprint_vmr(struct vmspace *vmspace)
{
    struct rb_node *node;
    struct vmregion *vmr;
    vaddr_t start, end;

    /* rb_for_each will iterate the vmrs in order. */
    read_lock(&vmspace->vmspace_lock);
    rb_for_each(&vmspace->vmr_tree, node)
    {
        vmr = rb_entry(node, struct vmregion, tree_node);
        start = vmr->start;
        end = start + vmr->size;
        kinfo("[%p] [vmregion] start=%p end=%p. vmr->pmo->type=%d\n",
              vmspace,
              start,
              end,
              vmr->pmo->type);
    }
    read_unlock(&vmspace->vmspace_lock);
}

/* This function should be surrounded with a lock (vmspace_lock). */
struct vmregion *find_vmr_for_va(struct vmspace *vmspace, vaddr_t addr)
{
    struct vmregion *vmr;
    struct rb_node *node;

    node = rb_search(&vmspace->vmr_tree, (const void *)addr, cmp_vmr_and_va);

    if (unlikely(node == NULL))
        return NULL;

    vmr = rb_entry(node, struct vmregion, tree_node);
    return vmr;
}

int vmspace_map_range(struct vmspace *vmspace, vaddr_t va, size_t len,
                      vmr_prop_t flags, struct pmobject *pmo, 
                      struct vmregion **out_vmregion)
{
    struct vmregion *vmr;
    int ret;

    /* Check whether the pmo type is supported */
    BUG_ON((pmo->type < PMO_ANONYM) || pmo->type > PMO_FORBID);

    /* Align a vmr to PAGE_SIZE */
    va = ROUND_DOWN(va, PAGE_SIZE);
    if (len < PAGE_SIZE)
        len = PAGE_SIZE;
    vmr = alloc_vmregion();
    if (!vmr) {
        ret = -ENOMEM;
        goto out_fail;
    }
    vmr->vmspace = (void *)vmspace;
    vmr->start = va;
    vmr->size = len;
    vmr->perm = flags;
    if (unlikely(pmo->type == PMO_DEVICE)) {
        vmr->perm |= VMR_DEVICE;
    } else if (unlikely(pmo->type == PMO_DATA_NOCACHE)) {
        vmr->perm |= VMR_NOCACHE;
    }
    vmr->pmo = pmo;
#ifdef RMAP_ENABLED
    /* Currently, one vmr has exactly one pmo */
    pmo_add_reverse_node(pmo, vmr);
#endif
    /*
     * Note that each operation on the vmspace should be protected by
     * the per_vmspace lock, i.e., vmspace_lock
     */
    write_lock(&vmspace->vmspace_lock);
    ret = add_vmr_to_vmspace(vmspace, vmr);
    write_unlock(&vmspace->vmspace_lock);

    if (ret < 0) {
        kwarn("add_vmr_to_vmspace fails, vmr->va=%llx, ->size=%llx\n",
              vmr->start,
              vmr->size);
        goto out_free_vmr;
    }

    /*
     * Case-1:
     * If the pmo type is PMO_DATA or PMO_DEVICE, we directly add mappings
     * in the page table because the corresponding physical pages are
     * prepared. In this case, early mapping avoids page faults and brings
     * better performance.
     *
     * Case-2:
     * Otherwise (for PMO_ANONYM and PMO_SHM), we use on-demand mapping.
     * In this case, lazy mapping reduces the usage of physical memory
     * resource.
     */
    if ((pmo->type == PMO_DATA) || (pmo->type == PMO_DATA_NOCACHE)
        || (pmo->type == PMO_DEVICE) || (pmo->type == PMO_RING_BUFFER))
        fill_page_table(vmspace, vmr);

    if (out_vmregion)
        *out_vmregion = vmr;

    /* On success */
    return 0;
out_free_vmr:
#ifdef RMAP_ENABLED
    pmo_remove_reverse_node(pmo, vmr);
#endif
    free_vmregion(vmr);
out_fail:
    return ret;
}

int vmspace_unmap_range(struct vmspace *vmspace, vaddr_t va, size_t len)
{
    struct vmregion *vmr;
    vaddr_t start;
    size_t size;
    struct pmobject *pmo;
    int ret;

    /*
     * Protect `find_vmr_for_va` and `del_vmr_from_vmspace`
     * with the vmspace_lock
     */
    write_lock(&vmspace->vmspace_lock);
    vmr = find_vmr_for_va(vmspace, va);
    if (!vmr) {
        kwarn("unmap a non-exist vmr.\n");
        ret = -1;
        goto out;
    }
    start = vmr->start;
    size = vmr->size;

    /* Sanity check: unmap the whole vmr */
    if (((va != start) || (len != size)) && (len != 0)) {
        kdebug("va: %p, start: %p, len: %p, size: %p\n", va, start, len, size);
        kwarn("Only support unmapping a whole vmregion now.\n");
        BUG_ON(1);
    }

    del_vmr_from_vmspace(vmspace, vmr);
    write_unlock(&vmspace->vmspace_lock);

    pmo = vmr->pmo;
    /* No pmo is mapped */
    if (pmo == NULL) {
        ret = 0;
        goto out;
    }

    /*
     * Remove the mappings in the page table.
     * When the pmo-type is DATA/DEVICE, each mapping must exist.
     *
     * Otherwise, the mapping is added on demand, which may not exist.
     * However, simply clearing non-present ptes is OK.
     */

    if (likely(len != 0)) {
        lock(&vmspace->pgtbl_lock);
        unmap_range_in_pgtbl(vmspace->pgtbl, va, len);
        unlock(&vmspace->pgtbl_lock);

        flush_tlbs(vmspace, va, len);
    }

    /*
     * Now, we defer the free of physical pages in the PMO
     * to the recycle procedure of a process.
     */

    ret = 0;
out:
    return ret;
}

static struct vmregion *find_vmr_by_pmo(struct vmspace *vmspace,
                                        struct pmobject *pmo)
{
    struct vmregion *vmr = NULL;
    struct vmregion *iter_vmr;

    /* Find the corresponding vmr of the given pmo */
    for_each_in_list (
            iter_vmr, struct vmregion, list_node, &vmspace->vmr_list) {
        if (iter_vmr->pmo == pmo) {
            vmr = iter_vmr;
            break;
        }
    }

#if 0
        /* We do not use rbtree iteration since the list interation could be faster. */
        bool found = false;
        struct rb_node *node;
        rb_for_each(vmspace->vmr_list, node) {
                vmr = rb_entry(node, struct vmregion, node);
                if (vmr->pmo == pmo) {
                        found = true;
                        break;
                }
        }

	if (!found) {
                vmr = NULL;
	}
#endif

    return vmr;
}

/*
 * Remove the mapping if a vmregion in the given vmspace points to the pmo.
 * If a process wants to map pmos to another process`s vmspace and
 * free these pmo_caps in its own cap group. It may use this function to
 * remove the mappings in its own vmspace
 */
int unmap_pmo_in_vmspace(struct vmspace *vmspace, struct pmobject *pmo)
{
    int ret;
    struct vmregion *vmr;

    u64 flush_va_start;
    u64 flush_len;

    write_lock(&vmspace->vmspace_lock);

    vmr = find_vmr_by_pmo(vmspace, pmo);

    if (vmr == NULL) {
        ret = -ENOENT;
        goto out;
    }

    flush_va_start = vmr->start;
    flush_len = vmr->size;
    /* Remove the vmr from the given vmspace */
    del_vmr_from_vmspace(vmspace, vmr);
    write_unlock(&vmspace->vmspace_lock);

    lock(&vmspace->pgtbl_lock);
    /* Remove the mapping in page table */
    unmap_range_in_pgtbl(vmspace->pgtbl, flush_va_start, flush_len);
    unlock(&vmspace->pgtbl_lock);

    flush_tlbs(vmspace, flush_va_start, flush_len);

    return 0;
out:
    write_unlock(&vmspace->vmspace_lock);
    return ret;
}

/* In the beginning, a vmspace ran on zero CPU */
static inline void reset_history_cpus(struct vmspace *vmspace)
{
    int i;

    for (i = 0; i < PLAT_CPU_NUM; ++i)
        vmspace->history_cpus[i] = 0;
}

void record_history_cpu(struct vmspace *vmspace, u32 cpuid)
{
    BUG_ON(cpuid >= PLAT_CPU_NUM);
    /*
     * Note that lock/atomic_ops are not required here
     * because only CPU X will modify (record/clear)
     * history_cpus[X].
     */
    vmspace->history_cpus[cpuid] = 1;
}

void clear_history_cpu(struct vmspace *vmspace, u32 cpuid)
{
    BUG_ON(cpuid >= PLAT_CPU_NUM);
    /*
     * Note that lock/atomic_ops are not required here
     * because only CPU X will modify (record/clear)
     * history_cpus[X].
     */
    vmspace->history_cpus[cpuid] = 0;
}

/*
 * The heap region of each process starts at HEAP_START and can at most grow
 * to (MMAP_START-1). (up to 16 TB)
 *
 * TODO: add guard vmr in between HEAP_START and MMAP_START.
 * TODO: add guard vmr for each thread's stack (which should be iniated by the
 * user library).
 *
 * The mmap region of each process starts at MMAP_START and can at most grow
 * to USER_SPACE_END. (up to 16 TB)
 *
 * For x86_64:
 * In 64-bit mode, an address is considered to be in canonical form
 * if address bits 63 through to the most-significant implemented bit
 * by the microarchitecture are set to either all ones or all zeros.
 * The kernel and user share the 48-bit address (0~2^48-1).
 * As usual, we let the kernel use the top half and the user use the
 * bottom half.
 * So, the user address is 0 ~ 2^47-1 (USER_SPACE_END).
 *
 */

// #define HEAP_START	(0x600000000000UL)
#define MMAP_START     (0x700000000000UL)
#define USER_SPACE_END (0x800000000000UL)

/* Each process has one heap_vmr. */
struct vmregion *init_heap_vmr(struct vmspace *vmspace, vaddr_t va,
                               struct pmobject *pmo)
{
    struct vmregion *vmr;

    vmr = alloc_vmregion();
    if (!vmr) {
        kwarn("%s fails\n", __func__);
        return NULL;
    }
    vmr->vmspace = (void *)vmspace;
    vmr->start = va;
    vmr->size = 0;
    vmr->perm = VMR_READ | VMR_WRITE;
    vmr->pmo = pmo;
#ifdef RMAP_ENABLED
    pmo_add_reverse_node(pmo, vmr);
#endif
    return vmr;
}

void adjust_heap_vmr(struct vmspace *vmspace, unsigned long add_len)
{
    struct vmregion *vmr;

    vmr = vmspace->heap_vmr;
    /*
     * During adjust, vmr->vmspace can be NULL.
     * PRE-MEMCPY thread might query vmr->vmspace->pgtable.
     */
#ifdef RMAP_ENABLED
    lock(&(vmr->pmo->reverse_list_lock));
#endif
    remove_vmr_from_vmspace(vmspace, vmr);
    vmr->size += add_len;
    vmr->pmo->size += add_len;
    add_vmr_to_vmspace(vmspace, vmr);

#ifdef RMAP_ENABLED
    unlock(&(vmr->pmo->reverse_list_lock));
#endif
}

u64 vmspace_mmap_with_pmo(struct vmspace *vmspace, struct pmobject *pmo,
                          size_t len, vmr_prop_t perm)
{
    struct vmregion *vmr;
    int ret;

    vmr = alloc_vmregion();
    if (!vmr) {
        kwarn("%s fails\n", __func__);
        goto out_fail;
    }

    vmr->vmspace = (void *)vmspace;

    /* Protect vmspace->user_current_mmap_addr with vmspace_lock */
    write_lock(&vmspace->vmspace_lock);

    vmr->start = vmspace->user_current_mmap_addr;

    BUG_ON(len % PAGE_SIZE);
    /* TODO: for simplicity, just keep increasing the mmap_addr now */
    vmspace->user_current_mmap_addr += len;
    vmr->size = len;
    vmr->perm = perm;

    /*
     * Currently, we restrict the pmo types, which must be
     * pmo_anonym or pmo_shm or pmo_file.
     */
    BUG_ON((pmo->type != PMO_ANONYM) && (pmo->type != PMO_SHM)
           && (pmo->type != PMO_FILE));

    vmr->pmo = pmo;
#ifdef RMAP_ENABLED
    pmo_add_reverse_node(pmo, vmr);
#endif
    ret = add_vmr_to_vmspace(vmspace, vmr);
    write_unlock(&vmspace->vmspace_lock);

    if (ret < 0)
        goto out_free_vmr;

    return vmr->start;

out_free_vmr:
#ifdef RMAP_ENABLED
    pmo_remove_reverse_node(pmo, vmr);
#endif
    free_vmregion(vmr);
out_fail:
    return (u64)-1L;
}

int vmspace_unmap_shm_vmr(struct vmspace *vmspace, vaddr_t va)
{
    struct vmregion *vmr;
    struct pmobject *pmo;

    u64 flush_va_start;
    u64 flush_len;

    write_lock(&vmspace->vmspace_lock);

    vmr = find_vmr_for_va(vmspace, va);
    if (vmr == NULL) {
        kwarn("%s: no vmr found for the va 0x%lx.\n", __func__, va);
        goto fail_out;
    }

    pmo = vmr->pmo;

    /* Sanity check */
    /* check-1: this interface is only used for shmdt */
    BUG_ON(pmo->type != PMO_SHM);
    /* check-2: the va should be the start address of the shm */
    BUG_ON(va != vmr->start);

    /*
     * Physical resources free should be done when the shm object is
     * removed by shmctl.
     */

    /* Delete the vmr from the vmspace */
    del_vmr_from_vmspace(vmspace, vmr);

    write_unlock(&vmspace->vmspace_lock);

    /* Umap a whole vmr */
    lock(&vmspace->pgtbl_lock);
    unmap_range_in_pgtbl(vmspace->pgtbl, vmr->start, vmr->size);
    flush_va_start = vmr->start;
    flush_len = vmr->size;
    unlock(&vmspace->pgtbl_lock);

    /* Flush TLBs without holding locks */
    flush_tlbs(vmspace, flush_va_start, flush_len);

    return 0;

fail_out:
    write_unlock(&vmspace->vmspace_lock);
    return -EINVAL;
}

#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
void *create_patch_pool()
{
    struct pte_patch_pool *pool;
#if defined CHCORE_SLS
    pool = (struct pte_patch_pool *)get_dram_pages(0);
#else
    pool = (struct pte_patch_pool *)get_cxl_pages(0);
#endif
    pool->count = 0;
    pool->next = NULL;
    return (void *)pool;
}
#endif

extern void arch_vmspace_init(struct vmspace *);

int vmspace_init(struct vmspace *vmspace)
{
    init_list_head(&vmspace->vmr_list);
    init_rb_root(&vmspace->vmr_tree);

    /* Allocate the root page table page */
#if defined USE_NVM && defined USE_DRAM
    vmspace->pgtbl = get_dram_pages(0);
#else
    vmspace->pgtbl = get_pages(0, __DEFAULT__);
#endif
    BUG_ON(vmspace->pgtbl == NULL);
    memset((void *)vmspace->pgtbl, 0, PAGE_SIZE);

    /* Architecture-dependent initilization */
    arch_vmspace_init(vmspace);

    /*
     * Note: acquire vmspace_lock before pgtbl_lock
     * when locking them together.
     */
    rwlock_init(&vmspace->vmspace_lock);
    lock_init(&vmspace->pgtbl_lock);

    /* The vmspace does not run on any CPU for now */
    reset_history_cpus(vmspace);

    /* Set the mmap area: this variable is protected by the vmspace_lock */
    vmspace->user_current_mmap_addr = MMAP_START;

#ifdef CHCORE_SLS
    /* Init pte_patch_pool */
    vmspace->pte_patch_pool = NULL;
#endif
    return 0;
}

#ifndef FBINFER
void vmspace_deinit(void *ptr)
{
    struct vmspace *vmspace;
    vmspace = (struct vmspace *)ptr;

#ifdef RMAP_ENABLED
    struct vmregion *vmr;
    struct vmregion *tmp;

    /*
     * Free each vmregion in vmspace->vmr_list.
     * Only invoked when a process exits. No need to acquire the lock.
     */
    for_each_in_list_safe (vmr, tmp, list_node, &vmspace->vmr_list) {
        pmo_remove_reverse_node(vmr->pmo, vmr);
        free_vmregion(vmr);
    }
#endif
    extern void free_page_table(void *);
    free_page_table(vmspace->pgtbl);
#if 0
        /* FIXME: TLB flush (PCID reusing) */
        extern void flush_tlb_of_vmspace(struct vmspace *);
        flush_tlb_of_vmspace(vmspace);
#endif
}
#endif

/*
 * This function clones a vmspace. The new vmspace has the same layout and
 * the data is directly copied. Some optimizations could be applied (e.g.
 * vmregions with no write permission can be reused, not copied.)
 */
int vmspace_clone(struct vmspace *dst_vmspace, struct vmspace *src_vmspace,
                  struct cap_group *dst_cap_group)
{
    struct vmregion *vmr;
    struct vmregion *tmp;
    struct vmregion *new_vmr;
    struct pmobject *new_pmo;
    int r;
    int cap;
    bool is_cow;

    write_lock(&src_vmspace->vmspace_lock);
    lock(&src_vmspace->pgtbl_lock);
    dst_vmspace->heap_vmr = NULL;

    for_each_in_list_safe (vmr, tmp, list_node, &(src_vmspace->vmr_list)) {
        /* Create new pmo */
        new_pmo = obj_alloc(TYPE_PMO, sizeof(struct pmobject), __DEFAULT__);
        if (!new_pmo) {
            r = -ENOMEM;
            goto out_fail;
        }
        r = pmo_clone(new_pmo, vmr->pmo, &is_cow);
        if (r < 0) {
            r = -ENOMEM;
            goto out_fail;
        }

        cap = cap_alloc(dst_cap_group, new_pmo, 0);
        if (cap < 0) {
            r = cap;
            goto out_fail;
        }
        /* Create new vmregion */
        new_vmr = alloc_vmregion();
        if (!new_vmr) {
            r = -ENOMEM;
            kwarn("%s fails\n", __func__);
            goto out_fail;
        }
        new_vmr->start = vmr->start;
        new_vmr->size = vmr->size;
        new_vmr->perm = vmr->perm;
        new_vmr->pmo = new_pmo;
#ifdef RMAP_ENABLED
        pmo_add_reverse_node(new_pmo, new_vmr);
#endif
        add_vmr_to_vmspace(dst_vmspace, new_vmr);

        /*
         * For PMO based on continous physical pages, we directly
         * map it in the page table. For PMO based on radix tree, it
         * will be automatically mapped when the page fault occurs.
         */
        if (use_continuous_pages(new_pmo))
            fill_page_table(dst_vmspace, new_vmr);

        if (vmr == src_vmspace->heap_vmr)
            dst_vmspace->heap_vmr = new_vmr;

        if (is_cow && (vmr->perm & VMR_WRITE)) {
            extern int set_write_in_pgtbl(struct vmspace * vmspace,
                                          vaddr_t va,
                                          size_t len,
                                          bool flag);
            if (use_continuous_pages(new_pmo))
                set_write_in_pgtbl(
                        dst_vmspace, new_vmr->start, new_vmr->size, false);
            set_write_in_pgtbl(src_vmspace, vmr->start, vmr->size, false);
        }
    }

    BUG_ON(dst_vmspace->heap_vmr == NULL);

    dst_vmspace->user_current_mmap_addr = src_vmspace->user_current_mmap_addr;

    r = 0;
out_fail:
    unlock(&src_vmspace->pgtbl_lock);
    write_unlock(&src_vmspace->vmspace_lock);
    return r;
}

/* Remove the physical page allocated to a pmo */
void remove_page_from_pmo(struct pmobject *pmo, u64 index)
{
    int ret;

    BUG_ON((pmo->type != PMO_ANONYM) && (pmo->type != PMO_SHM)
           && (pmo->type != PMO_FILE));

    ret = radix_del(pmo->radix, index);
    BUG_ON(ret != 0);
}

int pmo_copy(struct pmobject *src_pmo, struct pmobject *dst_pmo)
{
    int r;
    memset((void *)dst_pmo, 0, sizeof(*dst_pmo));
    dst_pmo->type = src_pmo->type;
    dst_pmo->size = src_pmo->size;

    switch (src_pmo->type) {
    case PMO_DATA:
    case PMO_RING_BUFFER:
        dst_pmo->start = src_pmo->start;
        break;
    case PMO_FILE: /* PMO backed by a file. It also uses the radix. */
    case PMO_ANONYM:
    case PMO_SHM:
    case PMO_RING_BUFFER_RADIX: {
        int phy_alloc = (src_pmo->type == PMO_SHM
                         || src_pmo->type == PMO_RING_BUFFER_RADIX) ?
                                1 :
                                0;
        dst_pmo->radix = new_radix();
        init_radix(dst_pmo->radix);
        r = radix_deep_copy(src_pmo->radix, dst_pmo->radix, phy_alloc);
        if (r) {
            kinfo("radix deep copy fail\n");
            goto out_fail;
        }
        break;
    }
    case PMO_DEVICE: {
        BUG_ON(1);
        dst_pmo->start = src_pmo->start;
        break;
    }
    case PMO_FORBID: {
        /* This type marks the corresponding area cannot be accessed */
        break;
    }
    default: {
        kinfo("Unsupported pmo type: %d\n", src_pmo->type);
        BUG_ON(1);
        break;
    }
    }
    return 0;
out_fail:
    return r;
}

#ifdef REPORT
extern u64 patch_page_num;
#endif
