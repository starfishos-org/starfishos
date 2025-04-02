#include <object/object.h>
#include <object/thread.h>
#include <object/memory.h>
#include <mm/vmspace.h>
#include <mm/uaccess.h>
#include <mm/mm.h>
#include <mm/kmalloc.h>
#include <common/lock.h>
#include <arch/mmu.h>
#include <common/lock.h>
#include <common/util.h>
#include <arch/mmu.h>
#include <object/user_fault.h>

#include "mmap.h"

extern int radix_deep_copy_with_hybird_mem(struct radix *src,
                                           struct radix *dst);

/*
 * Initialize an allocated pmobject.
 * @paddr is only used when @type == PMO_DEVICE.
 */
static int pmo_init(struct pmobject *pmo, pmo_type_t type, size_t len,
                    paddr_t paddr, mem_t mm_type)
{
    int ret = 0;

    memset((void *)pmo, 0, sizeof(*pmo));

    len = ROUND_UP(len, PAGE_SIZE);
    pmo->size = len;
    pmo->type = type;
    pmo->mm_type = mm_type;

#ifdef RMAP_ENABLED
    /* reverse list */
    init_list_head(&pmo->reverse_list);
    lock_init(&pmo->reverse_list_lock);
#endif
    switch (type) {
    case PMO_DATA:
    case PMO_DATA_NOCACHE:
    case PMO_RING_BUFFER:
    case PMO_CODE:
    {
        /*
         * For PMO_DATA, the user will use it soon (we expect).
         * So, we directly allocate the physical memory.
         * Note that kmalloc(>2048) returns continous physical pages.
         */
        void *new_va = kmalloc(len, pmo->mm_type);
        // kinfo("new_va: %lx\n", (u64)new_va);
        pmo->start = (paddr_t)virt_to_phys(new_va);

#if defined(CHCORE_SLS) && defined(RMAP_ENABLED)
        lock_init(&(pmo->dram_cache.lock));
        /* init first page's PMO info */
        struct page *page = virt_to_page(new_va);
        init_page_info(page, pmo, 0);
#endif
        break;
    }
    case PMO_FILE: {
#ifdef CHCORE_ENABLE_FMAP
        /*
         * PMO backed by a file.
         * We store PMO_FILE metadata in fmap_fault_pool in
         * pmo->private, and pmo->private is initialized NULL by memset.
         */
        struct fmap_fault_pool *pool_iter;
        u64 badge;
        badge = current_cap_group->badge;
        lock(&fmap_fault_pool_list_lock);
        for_each_in_list (pool_iter,
                          struct fmap_fault_pool,
                          node,
                          &fmap_fault_pool_list) {
            if (pool_iter->cap_group_badge == badge) {
                pmo->private = pool_iter;
                break;
            }
        }
        unlock(&fmap_fault_pool_list_lock);
        if (pmo->private == NULL) {
            /* fmap_fault_pool not registered */
            ret = -EINVAL;
            break;
        }
        pmo->radix = new_radix(__MT_OBJECT__);
        init_radix(pmo->radix);
#else
        kwarn("fmap is not implemented, we should not use PMO_FILE\n");
        ret = -EINVAL;
#endif
        break;
    }
    case PMO_ANONYM:
    case PMO_SHM:
    case PMO_RING_BUFFER_RADIX: 
    case PMO_STACK:
    case PMO_HEAP:
    case PMO_CROSS_SHM:
    {
        /*
         * For PMO_ANONYM (e.g., stack and heap) or PMO_SHM,
         * we do not allocate the physical memory at once.
         */
        pmo->radix = new_radix(__MT_OBJECT__);
        init_radix(pmo->radix);
        break;
    }
    case PMO_DEVICE: {
        /*
         * For device memory (e.g., for DMA).
         * We must ensure the range [paddr, paddr+len) is not
         * in the main memory region.
         */
        pmo->start = paddr;
        break;
    }
    case PMO_FORBID: {
        /* This type marks the corresponding area cannot be accessed */
        break;
    }
    default: {
        kinfo("Unsupported pmo type: %d\n", type);
        BUG_ON(1);
        break;
    }
    }
    return ret;
}

static int __create_pmo(u64 paddr, u64 size, u64 type, mem_t flags, 
            struct cap_group *cap_group, struct pmobject **new_pmo)
{
    int cap, r;
    struct pmobject *pmo;

    pmo = obj_alloc(TYPE_PMO, sizeof(*pmo), __MT_OBJECT__);
    if (!pmo) {
        r = -ENOMEM;
        goto out_fail;
    }

    BUG_ON(!IS_VALID_MEM_TYPE(flags));
    r = pmo_init(pmo, type, size, paddr, flags);
    if (r) {
        goto out_free_obj;
    }

    cap = cap_alloc(cap_group, pmo, 0);
    if (cap < 0) {
        r = cap;
        goto out_free_obj;
    }

    if (new_pmo != NULL)
        *new_pmo = pmo;

    return cap;

out_free_obj:
    obj_free(pmo);
out_fail:
    return r;
}

int create_device_pmo(u64 paddr, u64 size, struct pmobject **new_pmo)
{
    /* device pmo can not choose memory type */
    return __create_pmo(paddr, size, PMO_DEVICE, __MT_INVALID__,
            current_cap_group, new_pmo);
}

int sys_create_device_pmo(u64 paddr, u64 size)
{
    return create_device_pmo(paddr, size, NULL);
}

int create_pmo(u64 size, u64 type, mem_t flags, struct cap_group *cap_group,
               struct pmobject **new_pmo)
{
    return __create_pmo(0, size, type, flags, cap_group, new_pmo);
}

int sys_create_pmo(u64 size, u64 type, mem_t flags)
{
    BUG_ON(size == 0);
    return create_pmo(size, type, flags, current_cap_group, NULL);
}

struct pmo_request {
    /* args */
    u64 size;
    u64 type;
    /* return value */
    u64 ret_cap;
};

#define MAX_CNT 32

int sys_create_pmos(u64 user_buf, u64 cnt, int flags)
{
    u64 size;
    struct pmo_request *requests;
    int i;
    int cap;

    /* in case of integer overflow */
    if (cnt > MAX_CNT) {
        kwarn("create too many pmos for one time (max: %d)\n", MAX_CNT);
        return -EINVAL;
    }

    /* TODO: can we directly read/write user buffers */
    size = sizeof(*requests) * cnt;
    requests = (struct pmo_request *)kmalloc(size, flags);
    if (requests == NULL) {
        kwarn("cannot allocate more memory\n");
        return -EAGAIN;
    }
    copy_from_user((char *)requests, (char *)user_buf, size);

    for (i = 0; i < cnt; ++i) {
        cap = sys_create_pmo(requests[i].size, requests[i].type, flags);
        /*
         * TODO: what if some errors occur (i.e., create part of pmos).
         * levave it to user space for now.
         */
        requests[i].ret_cap = cap;
    }

    /* return pmo_caps */
    copy_to_user((char *)user_buf, (char *)requests, size);

    /* free temporary buffer */
    kfree(requests);

    return 0;
}

#define WRITE 0
#define READ  1
static int read_write_pmo(u64 pmo_cap, u64 offset, u64 user_buf, u64 size,
                          u64 op_type)
{
    /* TODO(MOK): what will happen during ckpt? */
    struct pmobject *pmo;
    pmo_type_t pmo_type;
    vaddr_t kva;
    int r;

    r = 0;
    /* Function caller should have the pmo_cap */
    pmo = obj_get(current_cap_group, pmo_cap, TYPE_PMO);
    if (!pmo) {
        r = -ECAPBILITY;
        goto out_fail;
    }

    /* Only PMO_DATA or PMO_ANONYM is allowed with this interface. */
    pmo_type = pmo->type;
    if ((pmo_type != PMO_DATA) && (pmo_type != PMO_DATA_NOCACHE)
        && (pmo_type != PMO_ANONYM) && (pmo_type != PMO_STACK)
        && (pmo_type != PMO_HEAP) && (pmo_type != PMO_CODE)) {
        r = -EINVAL;
        goto out_obj_put;
    }

    /* Only READ and WRITE operations are allowed. */
    if (op_type != READ && op_type != WRITE) {
        r = -EINVAL;
        goto out_obj_put;
    }

    /* Range check */
    if (offset + size < offset || offset + size > pmo->size) {
        r = -EINVAL;
        goto out_obj_put;
    }

    if (pmo_type == PMO_DATA || pmo_type == PMO_DATA_NOCACHE) {
        kva = phys_to_virt(pmo->start) + offset;
        if (op_type == WRITE)
            r = copy_from_user((char *)kva, (char *)user_buf, size);
        else // op_type == READ
            r = copy_to_user((char *)user_buf, (char *)kva, size);
    } else {
        /* PMO_ANONYM */
        u64 index;
        u64 pa;
        u64 to_read_write;
        u64 offset_in_page;

        while (size > 0) {
            index = ROUND_DOWN(offset, PAGE_SIZE) / PAGE_SIZE;
            pa = get_page_from_pmo(pmo, index);
            if (pa == 0) {
                /* Allocate a physical page for the anonymous
                 * pmo like a page fault happens.
                 */
                kva = (vaddr_t)get_pages(0, __MT_DEFAULT__);
                // kva = (vaddr_t)get_dram_pages(0);
                BUG_ON(kva == 0);

                pa = virt_to_phys((void *)kva);
                memset((void *)kva, 0, PAGE_SIZE);
                commit_page_to_pmo(pmo, index, pa);
                /* No need to map the physical page in the page
                 * table of current process because it uses
                 * write/read_pmo which means it does not need
                 * the mappings.
                 */
            } else {
                kva = phys_to_virt(pa);
            }
            /* Now kva is the beginning of some page, we should add
             * the offset inside the page. */
            offset_in_page = offset - ROUND_DOWN(offset, PAGE_SIZE);
            kva += offset_in_page;
            to_read_write = MIN(PAGE_SIZE - offset_in_page, size);

            if (op_type == WRITE)
                r = copy_from_user(
                        (char *)kva, (char *)user_buf, to_read_write);
            else // op_type == READ
                r = copy_to_user((char *)user_buf, (char *)kva, to_read_write);

            offset += to_read_write;
            size -= to_read_write;
        }
    }

out_obj_put:
    obj_put(pmo);
out_fail:
    return r;
}

/*
 * A process can send a PMO (with msgs) to others.
 * It can write the msgs without mapping the PMO with this function.
 */
int sys_write_pmo(u64 pmo_cap, u64 offset, u64 user_ptr, u64 len)
{
    return read_write_pmo(pmo_cap, offset, user_ptr, len, WRITE);
}

int sys_read_pmo(u64 pmo_cap, u64 offset, u64 user_ptr, u64 len)
{
    return read_write_pmo(pmo_cap, offset, user_ptr, len, READ);
}

/**
 * Given a pmo_cap, return its corresponding start physical address.
 */
int sys_get_pmo_paddr(u64 pmo_cap, u64 user_buf)
{
    struct pmobject *pmo;
    int r = 0;

    /* Caller should have the pmo_cap */
    pmo = obj_get(current_cap_group, pmo_cap, TYPE_PMO);
    if (!pmo) {
        r = -ECAPBILITY;
        goto out_fail;
    }

    /* Only allow to get the address of PMO_DATA for now */
    if (pmo->type != PMO_DATA && pmo->type != PMO_DATA_NOCACHE) {
        r = -EINVAL;
        goto out_obj_put;
    }

    copy_to_user((char *)user_buf, (char *)&pmo->start, sizeof(u64));

out_obj_put:
    obj_put(pmo);
out_fail:
    return r;
}

/* TreeSLS */
void page_refcnt_add(vaddr_t va, paddr_t pa)
{
    struct page *p_page;
    p_page = virt_to_page((void *)phys_to_virt(pa));
    atomic_fetch_add_64(&p_page->ref_cnt, 1);
}

/* For fork */
int pmo_clone(struct pmobject *dst_pmo, struct pmobject *src_pmo, bool *is_cow)
{
    int r = 0, i;
    int page_num;
    u64 *array;
    struct page *page;

    if (src_pmo == NULL || dst_pmo == NULL) {
        return -EINVAL;
    }

    *is_cow = true;

    dst_pmo->size = src_pmo->size;
    dst_pmo->type = src_pmo->type;
#ifdef RMAP_ENABLED
    init_list_head(&dst_pmo->reverse_list);
    lock_init(&dst_pmo->reverse_list_lock);
#endif
    switch (src_pmo->type) {
    case PMO_DATA:
    case PMO_DATA_NOCACHE: {
        lock_init(&(dst_pmo->dram_cache.lock));

        if (src_pmo->dram_cache.array != NULL) {
            /* Just copy */
            *is_cow = false;
            void *new_va = kmalloc(dst_pmo->size, __MT_DEFAULT__);
            if (new_va == NULL) {
                return -ENOMEM;
            }
            dst_pmo->start = (paddr_t)virt_to_phys(new_va);
            page = virt_to_page(new_va);
#if defined(CHCORE_SLS) && defined(HYBRID_MEM)
            init_page_info(page, dst_pmo, 0);
#endif
            array = src_pmo->dram_cache.array;
            page_num = DIV_ROUND_UP(src_pmo->size, PAGE_SIZE);
            for (i = 0; i < page_num; i++) {
                u64 src_pa, dst_pa = dst_pmo->start + i * PAGE_SIZE;
                if (array[i] != 0) {
                    src_pa = array[i];
                } else {
                    src_pa = src_pmo->start + i * PAGE_SIZE;
                }
                memcpy((void *)phys_to_virt(dst_pa),
                       (void *)phys_to_virt(src_pa),
                       PAGE_SIZE);
            }
        } else {
            /* Copy on write */
            dst_pmo->start = src_pmo->start;
            page = virt_to_page((void *)phys_to_virt(src_pmo->start));
            atomic_fetch_add_64(&page->ref_cnt, 1);
        }
        break;
    }
    case PMO_FILE: {
#ifdef CHCORE_ENABLE_FMAP
        /* PMO backed by a file. It also uses the radix. */
        dst_pmo->private = src_pmo->private;
#else
        kwarn("fmap is not implemented, we should not use PMO_FILE\n");
        r = -EINVAL;
        break;
#endif
    }
    case PMO_ANONYM: {
        /*
         * For radix tree based PMO, rebuild the radix tree.
         * The new radix tree should have the same structure.
         */
        dst_pmo->radix = new_radix(__MT_OBJECT__);
        init_radix(dst_pmo->radix);
#if defined(CHCORE_SLS) && defined(HYBRID_MEM)
        r = radix_deep_copy_with_hybird_mem(src_pmo->radix, dst_pmo->radix);
#else
        r = radix_deep_copy(src_pmo->radix, dst_pmo->radix, 0, __MT_OBJECT__);

#endif /* CHCHORE_SLS */
        if (r) {
            kinfo("radix_deep_copy failed: %d\n", r);
            break;
        }
        break;
    }
#ifdef USE_CXL_MEM
    case PMO_CROSS_SHM:
#endif
    case PMO_SHM:
    case PMO_RING_BUFFER_RADIX: {
        /*
         * For radix tree based PMO, rebuild the radix tree.
         * The new radix tree should have the same structure.
         */
        dst_pmo->radix = new_radix(__MT_OBJECT__);
        init_radix(dst_pmo->radix);
        r = radix_deep_copy(src_pmo->radix, dst_pmo->radix, false, __MT_OBJECT__);
        if (r) {
            kinfo("radix_deep_copy failed: %d\n", r);
            break;
        }

        radix_traverse(dst_pmo->radix, page_refcnt_add);

        break;
    }
    case PMO_RING_BUFFER:
    case PMO_DEVICE: {
        /* Device memory should be the same. */
        dst_pmo->start = src_pmo->start;
        break;
    }
    case PMO_FORBID: {
        /* This type marks the corresponding area cannot be accessed */
        dst_pmo->start = src_pmo->start;
        break;
    }
    default: {
        kinfo("Unsupported pmo type: %d\n", src_pmo->type);
        BUG_ON(1);
        break;
    }
    }

    if (is_shared_pmo(src_pmo))
        *is_cow = false;

    return r;
}

/*
 * Given a virtual address, return its corresponding physical address.
 * Notice: the virtual address should be page-backed, thus pre-fault could be
 * conducted before using this syscall.
 */
int sys_get_phys_addr(u64 va, u64 *pa_buf)
{
    struct vmspace *vmspace = current_thread->vmspace;
    paddr_t pa;
    int ret;

    lock(&vmspace->pgtbl_lock);
    extern int query_in_pgtbl(void *, vaddr_t, paddr_t *, void **);
    ret = query_in_pgtbl(vmspace->pgtbl, va, &pa, NULL);
    unlock(&vmspace->pgtbl_lock);

    if (ret < 0)
        return ret;

    copy_to_user((char *)pa_buf, (char *)&pa, sizeof(u64));

    return 0;
}

int trans_uva_to_kva(u64 user_va, u64 *kernel_va)
{
    struct vmspace *vmspace = current_thread->vmspace;
    paddr_t pa;
    int ret;

    lock(&vmspace->pgtbl_lock);
    extern int query_in_pgtbl(void *, vaddr_t, paddr_t *, void **);
    ret = query_in_pgtbl(vmspace->pgtbl, user_va, &pa, NULL);
    unlock(&vmspace->pgtbl_lock);

    if (ret < 0)
        return ret;

    *kernel_va = phys_to_virt(pa);
    return 0;
}

/*
 * A process can not only map a PMO into its private address space,
 * but also can map a PMO to some others (e.g., load code for others).
 */
int sys_map_pmo(u64 target_cap_group_cap, u64 pmo_cap, u64 addr, u64 perm,
                u64 len)
{
    struct vmspace *vmspace;
    struct pmobject *pmo;
    struct cap_group *target_cap_group;
    int r;

    pmo = obj_get(current_cap_group, pmo_cap, TYPE_PMO);
    if (!pmo) {
        r = -ECAPBILITY;
        goto out_fail;
    }

    /* translate default length (-1) to pmo_size */
    if (likely(len == -1))
        len = pmo->size;

    /* map the pmo to the target cap_group */
    target_cap_group =
            obj_get(current_cap_group, target_cap_group_cap, TYPE_CAP_GROUP);
    if (!target_cap_group) {
        r = -ECAPBILITY;
        goto out_obj_put_pmo;
    }
    vmspace = obj_get(target_cap_group, VMSPACE_OBJ_ID, TYPE_VMSPACE);
    BUG_ON(vmspace == NULL);

    // TODO: mapping device with huge pages?

    /*
     * TODO (question): is it required to restrict pmo mapping and how?
     * - check wheter perm is legal?
     * - check addr validation?
     */
    r = vmspace_map_range(vmspace, addr, len, perm, pmo, NULL);
    if (r != 0) {
        r = -EPERM;
        goto out_obj_put_vmspace;
    }

    /*
     * when a process maps a pmo to others,
     * this func returns the new_cap in the target process.
     */
    if (target_cap_group != current_cap_group)
        /* if using cap_move, we need to consider remove the mappings */
        r = cap_copy(current_cap_group, target_cap_group, pmo_cap);
    else
        r = 0;

out_obj_put_vmspace:
    obj_put(vmspace);
    obj_put(target_cap_group);
out_obj_put_pmo:
    obj_put(pmo);
out_fail:
    return r;
}

/* Example usage: Used in ipc/connection.c for mapping ipc_shm */
int map_pmo_in_current_cap_group(u64 pmo_cap, u64 addr, u64 perm)
{
    struct vmspace *vmspace;
    struct pmobject *pmo;
    int r;

    pmo = obj_get(current_cap_group, pmo_cap, TYPE_PMO);
    if (!pmo) {
        kinfo("map fails: invalid pmo (cap is %lu)\n", pmo_cap);
        r = -ECAPBILITY;
        goto out_fail;
    }

    vmspace = obj_get(current_cap_group, VMSPACE_OBJ_ID, TYPE_VMSPACE);
    BUG_ON(vmspace == NULL);
    r = vmspace_map_range(vmspace, addr, pmo->size, perm, pmo, NULL);
    if (r != 0) {
        kinfo("%s failed: addr 0x%lx, pmo->size 0x%lx\n",
              __func__,
              addr,
              pmo->size);
        r = -EPERM;
        goto out_obj_put_vmspace;
    }

out_obj_put_vmspace:
    obj_put(vmspace);
    obj_put(pmo);
out_fail:
    return r;
}

struct pmo_map_request {
    /* args */
    u64 pmo_cap;
    u64 addr;
    u64 perm;
    /*
     * If you want to free the pmo cap in current cap goup
     * after the pmo was mapped to the vmsapce of another
     * process, please set free_cap to 1.
     */
    u64 free_cap;

    /* return caps or return value */
    u64 ret;
};

int sys_map_pmos(u64 target_cap_group_cap, u64 user_buf, u64 cnt)
{
    u64 size;
    struct pmo_map_request *requests;
    struct vmspace *vmspace;
    struct pmobject *pmo;
    int i;
    int map_ret, ret = 0;

    /* in case of integer overflow */
    if (cnt > MAX_CNT) {
        kwarn("create too many pmos for one time (max: %d)\n", MAX_CNT);
        return -EINVAL;
    }

    /* TODO: can we directly read/write user buffers */
    size = sizeof(*requests) * cnt;
    requests = (struct pmo_map_request *)kmalloc(size, __MT_DEFAULT__);
    if (requests == NULL) {
        kwarn("cannot allocate more memory\n");
        return -EAGAIN;
    }
    copy_from_user((char *)requests, (char *)user_buf, size);

    for (i = 0; i < cnt; ++i) {
        /*
         * if target_cap_group is not current_cap_group,
         * ret is cap on success.
         */
        map_ret = sys_map_pmo(target_cap_group_cap,
                              requests[i].pmo_cap,
                              requests[i].addr,
                              requests[i].perm,
                              -1 /* pmo size */);
        requests[i].ret = map_ret;

        /*
         * One failure will not abort the following request.
         * Leave user space to handle partial failure.
         */
        if (map_ret < 0)
            ret = -EINVAL;

        if (ret >= 0 && requests[i].free_cap == 1) {
            pmo = obj_get(current_cap_group, requests[i].pmo_cap, TYPE_PMO);
            vmspace = obj_get(current_cap_group, VMSPACE_OBJ_ID, TYPE_VMSPACE);
            BUG_ON(pmo == NULL || vmspace == NULL);
            /*
             * If the pmo being freed is mapped to a
             * vmregion in current vmspace, we need
             * to remove the mapping.
             */
            unmap_pmo_in_vmspace(vmspace, pmo);

            cap_free(current_cap_group, requests[i].pmo_cap);
            obj_put(vmspace);
            obj_put(pmo);
        }
    }

    copy_to_user((char *)user_buf, (char *)requests, size);

    kfree(requests);
    return ret;
}

/* TODO: add sys_unmap_pmos */
int sys_unmap_pmo(u64 target_cap_group_cap, u64 pmo_cap, u64 addr)
{
    struct vmspace *vmspace;
    struct pmobject *pmo;
    struct cap_group *target_cap_group;
    int ret;

    /* caller should have the pmo_cap */
    pmo = obj_get(current_cap_group, pmo_cap, TYPE_PMO);
    if (!pmo)
        return -EPERM;

    /* map the pmo to the target cap_group */
    target_cap_group =
            obj_get(current_cap_group, target_cap_group_cap, TYPE_CAP_GROUP);
    if (!target_cap_group) {
        ret = -EPERM;
        goto fail1;
    }

    vmspace = obj_get(target_cap_group, VMSPACE_OBJ_ID, TYPE_VMSPACE);
    if (!vmspace) {
        ret = -EPERM;
        goto fail2;
    }

    // TODO (question): is it required to restrict pmo mapping?
    ret = vmspace_unmap_range(vmspace, addr, pmo->size);
    if (ret != 0)
        ret = -EPERM;

    obj_put(vmspace);
fail2:
    obj_put(target_cap_group);
fail1:
    obj_put(pmo);

    return ret;
}

void commit_dram_cached_page(struct pmobject *pmo, u64 index, paddr_t pa)
{
    if (is_continuous_pmo(pmo)) {
        /* alloc dram cached array is NULL */
        lock(&(pmo->dram_cache.lock));
        if (!pmo->dram_cache.array) {
            pmo->dram_cache.array = temp_kmalloc(
                    DIV_ROUND_UP(pmo->size, PAGE_SIZE) * sizeof(u64));
        }
        unlock(&(pmo->dram_cache.lock));
        BUG_ON(index >= DIV_ROUND_UP(pmo->size, PAGE_SIZE));
        pmo->dram_cache.array[index] = pa;
    } else if (is_radix_pmo(pmo)) {
        BUG_ON(radix_add(pmo->radix, index, (void *)pa));
    } else {
        BUG("Unsupport pmo type\n");
    }
#if defined(CHCORE_SLS) && defined(HYBRID_MEM)
    struct page *page = virt_to_page((void *)phys_to_virt(pa));
    init_page_info(page, pmo, index);
#endif
}

void clear_dram_cached_page(struct pmobject *pmo, u64 index)
{
    BUG_ON(!is_continuous_pmo(pmo));
    BUG_ON(index >= DIV_ROUND_UP(pmo->size, PAGE_SIZE));
    BUG_ON(!pmo->dram_cache.array);
    if (pmo->dram_cache.array[index] == 0)
        BUG("pmo=%p, index=%d\n is not cached in pmo\n", pmo, index);
    pmo->dram_cache.array[index] = 0;
}

void clear_dram_cache(struct pmobject *pmo)
{
    BUG_ON(!is_continuous_pmo(pmo));
    BUG_ON(!pmo->dram_cache.array);
    kfree(pmo->dram_cache.array);
    pmo->dram_cache.array = NULL;
}

/* Record the physical page allocated to a pmo */
void commit_page_to_pmo(struct pmobject *pmo, u64 index, paddr_t pa)
{
    /* commit nvm/dram page to radix pmo */
    BUG_ON(!is_radix_pmo(pmo));
    /* The radix interfaces are thread-safe */
    BUG_ON(radix_add(pmo->radix, index, (void *)pa));

#ifdef RMAP_ENABLED
    struct page *page = virt_to_page((void *)phys_to_virt(pa));
    init_page_info(page, pmo, index);
#endif
}

/* Return 0 (NULL) when not found */
paddr_t get_page_from_pmo(struct pmobject *pmo, u64 index)
{
    paddr_t pa = 0;

    if (is_radix_pmo(pmo)) {
        /* The radix interfaces are thread-safe */
        if (!pmo->radix) {
            BUG("pmo type: %d with radix is NULL\n", pmo->type);
        }
        pa = (paddr_t)radix_get(pmo->radix, index);
    } else if (is_continuous_pmo(pmo)) {
#if defined(CHCORE_SLS) && defined(HYBRID_MEM)
        if (pmo->dram_cache.array)
            pa = pmo->dram_cache.array[index];
        /* pa is not dram cached */
        if (pa == 0)
            pa = pmo->start + index * PAGE_SIZE;
#else
        pa = pmo->start + index * PAGE_SIZE;
#endif
    } else {
        BUG("Not supported type\n");
    }
    return pa;
}

static void __free_pmo_page(void *addr)
{
    kfree((void *)phys_to_virt(addr));
}

void pmo_deinit(void *pmo_ptr)
{
    struct pmobject *pmo;
    pmo_type_t type;

    pmo = (struct pmobject *)pmo_ptr;
    type = pmo->type;

    if (is_continuous_pmo(pmo)) {
        paddr_t start_addr;

        /* PMO_DATA contains continous physical pages */
        start_addr = pmo->start;
        kfree((void *)phys_to_virt(start_addr));
    } else if (is_radix_pmo(pmo)) {
        struct radix *radix;

        radix = pmo->radix;
        BUG_ON(radix == NULL);
        /*
         * Set value_deleter to free each memory page during
         * traversing the radix tree in radix_free.
         */
        radix->value_deleter = __free_pmo_page;
        radix_free(radix);
    } else if (is_unchangeable_pmo(pmo)) {
        /* Do nothing */
    } else {
        BUG("Unsupported pmo type: %d\n", type);
    }
#ifdef RMAP_ENABLED
    /* free reverse list */
    struct reverse_node *rnode, *tmp;
    for_each_in_list_safe (rnode, tmp, node, &pmo->reverse_list) {
        list_del(&(rnode->node));
        kfree(rnode);
    }
#endif /* RMAP_ENABLED */
    /* The pmo struct itself will be free in __free_object */
}

/*
 * User process heap start: 0x600000000000 (i.e., HEAP_START)
 *
 * defined in mm/vmregion.c
 */

/*
 * TODO (tmac): we should modify LibC malloc as follows:
 * instead of invoking brk(0) at first, it should create the heap pmo by itself.
 */
u64 sys_handle_brk(u64 addr, u64 heap_start, int flags)
{
    struct vmspace *vmspace;
    struct pmobject *pmo;
    struct vmregion *heap_vmr;
    size_t len;
    u64 retval = 0;
    int pmo_cap;

    vmspace = obj_get(current_cap_group, VMSPACE_OBJ_ID, TYPE_VMSPACE);
    write_lock(&vmspace->vmspace_lock);
    // printk("sys_handle_brk enter\n");
    if (addr == 0) {
        retval = heap_start;

        /* create the heap pmo for the user process */
        len = 0;
        pmo_cap = create_pmo(len, PMO_HEAP, flags, current_cap_group, &pmo);
        if (pmo_cap < 0) {
            kinfo("Fail: cannot create the initial heap pmo.\n");
            BUG_ON(1);
        }

        /* setup the vmr for the heap region */
        heap_vmr = init_heap_vmr(vmspace, retval, pmo);
        if (!heap_vmr) {
            kinfo("Fail: cannot create the initial heap pmo.\n");
            BUG_ON(1);
        }
        vmspace->heap_vmr = heap_vmr;
    } else {
        heap_vmr = vmspace->heap_vmr;
        if (unlikely(heap_vmr == NULL))
            goto out;

        /* old heap end */
        retval = heap_vmr->start + heap_vmr->size;

        if (addr >= retval) {
            /* enlarge the heap vmr and pmo */
            len = addr - retval;
            adjust_heap_vmr(vmspace, len);
            retval = addr;
        } else {
            kwarn("VM: ignore shrinking the heap.\n");
        }
    }

out:
    // printk("sys_handle_brk finish\n");
    write_unlock(&vmspace->vmspace_lock);
    obj_put(vmspace);
    return retval;
}

/* A process mmap region start:  MMAP_START (defined in mm/vmregion.c) */
static vmr_prop_t get_vmr_prot(int prot)
{
    vmr_prop_t ret;

    ret = 0;
    if (prot & PROT_READ)
        ret |= VMR_READ;
    if (prot & PROT_WRITE)
        ret |= VMR_WRITE;
    if (prot & PROT_EXEC)
        ret |= VMR_EXEC;

    return ret;
}

extern u64 vmspace_mmap_with_pmo(struct vmspace *vmspace, struct pmobject *pmo,
                                 size_t len, vmr_prop_t perm);
/* vmr_prot must contains target_prot */
static int valid_prot(vmr_prop_t vmr_prot, vmr_prop_t target_prot)
{
    if ((target_prot & PROT_READ) && !(vmr_prot & PROT_READ))
        return -1;

    if ((target_prot & PROT_WRITE) && !(vmr_prot & PROT_WRITE))
        return -1;

    if ((target_prot & PROT_EXEC) && !(vmr_prot & PROT_EXEC))
        return -1;

    return 0;
}

extern int mprotect_in_pgtbl(void *, vaddr_t, size_t, vmr_prop_t);

int sys_handle_mprotect(u64 addr, size_t length, int prot)
{
    vmr_prop_t target_prot;
    struct vmspace *vmspace;
    struct vmregion *vmr;
    s64 remaining;
    u64 va;
    int ret;

    if ((addr % PAGE_SIZE) || (length % PAGE_SIZE)) {
        return -EINVAL;
    }

    if (length == 0)
        return 0;

    target_prot = get_vmr_prot(prot);
    vmspace = obj_get(current_cap_group, VMSPACE_OBJ_ID, TYPE_VMSPACE);

    write_lock(&vmspace->vmspace_lock);
    /*
     * Validate the VM range [addr, addr + lenght]
     * - the range is totally mapped
     * - the range has more permission than prot
     */
    va = addr;
    remaining = length;
    while (remaining > 0) {
        vmr = find_vmr_for_va(vmspace, va);

        if (!vmr) {
            ret = -EACCES;
            goto out;
        }

        if (valid_prot(vmr->perm, target_prot)) {
            ret = -EACCES;
            goto out;
        }

        if (remaining < vmr->size) {
            static int warn = 1;

            if (warn == 1) {
                kwarn("func: %s, ignore a mprotect since no supporting for "
                      "splitting vmr now.\n",
                      __func__);
                warn = 0;
            }

            // TODO: mprotect a part of a vmr
            // no support splitting the region now
            // ret = -EINVAL;
            // goto out;

            // TODO: do nothing
            // passing valid_prot means permission reducing
            // just igonre this syscall now

            obj_put(vmspace);
            write_unlock(&vmspace->vmspace_lock);

            ret = 0;
            return ret;
        }

        remaining -= vmr->size;
        va += vmr->size;
    }

    BUG_ON(remaining != 0);

#ifndef FBINFER
    /* Change the prot in each vmr */
    va = addr;
    remaining = length;

    while (remaining > 0) {
        vmr = find_vmr_for_va(vmspace, va);
        vmr->perm = target_prot;

        remaining -= vmr->size;
        va += vmr->size;
    }
#endif

    /* Modify the existing mappings in pgtbl */
    lock(&vmspace->pgtbl_lock);
    mprotect_in_pgtbl(vmspace->pgtbl, addr, length, target_prot);
    unlock(&vmspace->pgtbl_lock);
    ret = 0;

out:
    obj_put(vmspace);
    write_unlock(&vmspace->vmspace_lock);

    return ret;
}

/* TODO: This syscall should be optimized (deleted) */
u64 sys_map_with_pmo(u64 pmo_cap, u64 perm)
{
    struct vmspace *vmspace;
    struct pmobject *pmo;
    u64 addr;

    vmspace = obj_get(current_cap_group, VMSPACE_OBJ_ID, TYPE_VMSPACE);
    pmo = obj_get(current_cap_group, pmo_cap, TYPE_PMO);
    if (!pmo) {
        addr = -ECAPBILITY;
        goto out;
    }
    /* Use the interface 'vmspace_mmap_with_pmo' */
    addr = vmspace_mmap_with_pmo(vmspace, pmo, pmo->size, perm);

out:
    obj_put(pmo);
    obj_put(vmspace);
    return addr;
}

extern int vmspace_unmap_shm_vmr(struct vmspace *, vaddr_t);
int sys_unmap_with_addr(u64 shmaddr)
{
    struct vmspace *vmspace;
    int ret;

    /*
     * TODO: only a temporary impl.
     * Now just unmap the whole corresponding vmr of shmaddr.
     */
    vmspace = obj_get(current_cap_group, VMSPACE_OBJ_ID, TYPE_VMSPACE);
    ret = vmspace_unmap_shm_vmr(vmspace, shmaddr);
    obj_put(vmspace);

    return ret;
}

u64 sys_get_free_mem_size(void)
{
    return get_free_mem_size();
}

inline bool is_unchangeable_pmo(struct pmobject *pmo)
{
    u64 type = pmo->type;
    return (type == PMO_DEVICE || type == PMO_FORBID);
}

inline bool is_radix_pmo(struct pmobject *pmo)
{
    u64 type = pmo->type;
    return (type != PMO_FORBID && !is_continuous_pmo(pmo) && !is_unchangeable_pmo(pmo));
}

inline bool is_continuous_pmo(struct pmobject *pmo)
{
    u64 type = pmo->type;
    return (type == PMO_DATA || 
            type == PMO_DATA_NOCACHE ||
            type == PMO_RING_BUFFER ||
            type == PMO_CODE);
}

inline bool is_external_sync_pmo(struct pmobject *pmo)
{
    return (pmo->type == PMO_RING_BUFFER || 
            pmo->type == PMO_RING_BUFFER_RADIX || 
            pmo->type == PMO_DEVICE);
}

inline bool is_shared_pmo(struct pmobject *pmo)
{
    if (pmo->type == PMO_SHM
#ifdef USE_CXL_MEM
        || pmo->type == PMO_CROSS_SHM
#endif
#ifdef CHCORE_SLS
        || is_external_sync_pmo(pmo)
#endif
    )
        return true;
    return false;
}
