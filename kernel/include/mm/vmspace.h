#pragma once

#include <common/list.h>
#include <common/lock.h>
#include <common/radix.h>
#include <arch/mmu.h>
#include <object/cap_group.h>

#ifdef CHCORE
#include <machine.h>
#endif

#include <common/rbtree.h>

/*
 * The anon_vma heads a list of private "related" vmas.
 *
 * Since vmas come and go as they are split and merged (particularly
 * in mprotect), the mapping field of an anonymous page cannot point
 * directly to a vma: instead it points to an anon_vma, on whose list
 * the related vmas can be easily linked or unlinked.
 */
struct virtual_vmregion {
    /* W: modification, R: walking the list */
    struct lock lock;
    /* Interval tree of private "related" vmrs */
#ifdef ENABLE_SPLIT_AND_MERGE
    u64 vmr_count;
    struct vmr_list {
        void *vmr;
        struct vmr_list *next;
    } list;
#else
    void *vmr;
#endif
};

// #define REVERSE_CACED 0x1

/*
 * `struct page` will maintain a list of reverse nodes which tracks
 * all the vmregions this page belongs to.
 *
 * We use a new virtual_vmregion structure. we not cache vmregion
 * here directly since vmregion can be  split or merge, and traverse
 * all `reverse_node` which point to this vmregion and change the
 * corresponding page lists is a significant cost operation.
 *
 * We also can not cache page index in `struct page` since vmregion
 * may changed and the index is invalid.
 */
struct reverse_node {
    struct list_head node; /* As one node of pmo->reverse_list */

    /*
     * Currently, chcore do not allow split/merge vmregion
     * But vmregion can be free, still use virtual vmr to track
     */
    // struct virtual_vmregion *virt_vmr;
    struct vmregion *vmr;
};

struct vmregion {
    struct list_head list_node; /* As one node of the vmr_list */
    struct rb_node tree_node; /* As one node of the vmr_tree */

    /* virtual vmr this vmregion belong to */
    // struct virtual_vmregion *virtual_vmr;

    /* vmspace belongs to */
    void *vmspace;

    vaddr_t start;
    size_t size;
    vmr_prop_t perm;
    struct pmobject *pmo;
};

#define VM_FLAG_PRESERVE \
    1 /* pages in the vmspace are referenced by some checkpoints */

struct vmspace {
    /* List head of vmregion (vmr_list) */
    struct list_head vmr_list;
    /* rbtree root node of vmregion (vmr_tree) */
    struct rb_root vmr_tree;

    /* Root page table */
    void *pgtbl;

    u64 pcid;

    /* The lock for manipulating vmregions */
    struct rwlock vmspace_lock;
    /* The lock for manipulating the page table */
    struct lock pgtbl_lock;

/*
 * For TLB flushing:
 * Record the all the CPU that a vmspace ran on.
 */
#ifdef CHCORE
    u8 history_cpus[PLAT_CPU_NUM];
#endif

    struct vmregion *heap_vmr;

    /* For the virtual address of mmap */
    vaddr_t user_current_mmap_addr;
#if defined(CHCORE_SLS) || defined(CHCORE_SSI_SLS)
    /* Track all modified pages('s pte) */
    void *pte_patch_pool;
#endif
    u64 flags;
};

typedef u64 pmo_type_t;
#define PMO_ANONYM            0 /* lazy allocation */
#define PMO_DATA              1 /* immediate allocation */
#define PMO_FILE              2 /* file backed */
#define PMO_SHM               3 /* shared memory */
#define PMO_USER_PAGER        4 /* support user pager */
#define PMO_DEVICE            5 /* memory mapped device registers */
#define PMO_DATA_NOCACHE      6 /* non-cacheable immediate allocation */
#define PMO_RING_BUFFER       7 /* pages that need to sync with external */
#define PMO_RING_BUFFER_RADIX 8 /* same as PMO_RING_BUFFER; for test*/
#define PMO_CROSS_SHM         9 /* shared memory accross machine */
#define PMO_FORBID            10 /* Forbidden area: avoid overflow */

#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
struct page_patch {
    // unsigned char type;
    struct list_head list_node;
    u64 offset;
    // void *page_data;
};

enum page_patch_type { PPATCH_NEW, PPATCH_MODIFY };
#endif /* CHCORE_SLS */

struct pmobject {
    union {
        struct radix *radix;
        struct dram_cache {
            u64 *array;
            struct lock lock;
        } dram_cache;
    };

    paddr_t start;
    size_t size;
    pmo_type_t type;

    /*
     * 'private' depends on 'type'.
     * PMO_FILE: it points to fmap_fault_pool
     * others: NULL
     */
    void *private;
#ifdef RMAP_ENABLED
    struct list_head reverse_list;
    struct lock reverse_list_lock;
#endif /* RMAP_ENABLED */
};

/* explore for ckpt/restore */
struct vmregion *alloc_vmregion(void);
void free_vmregion(struct vmregion *vmr);
int add_vmr_to_vmspace(struct vmspace *vmspace, struct vmregion *vmr);

struct vmspace *get_current_vmspace();
int vmspace_init(struct vmspace *vmspace);

struct cap_group;
int create_pmo(u64 size, u64 type, int flags, struct cap_group *cap_group,
               struct pmobject **new_pmo);
int create_device_pmo(u64 paddr, u64 size, struct pmobject **new_pmo);

int pmo_copy(struct pmobject *src_pmo, struct pmobject *dst_pmo);

int vmspace_map_range(struct vmspace *vmspace, vaddr_t va, size_t len,
                      vmr_prop_t flags, struct pmobject *pmo,
                      struct vmregion **out_vmr);
int vmspace_unmap_range(struct vmspace *vmspace, vaddr_t va, size_t len);
int unmap_pmo_in_vmspace(struct vmspace *vmspace, struct pmobject *pmo);

struct vmregion *find_vmr_for_va(struct vmspace *vmspace, vaddr_t addr);

void switch_vmspace_to(struct vmspace *);

void commit_page_to_pmo(struct pmobject *pmo, u64 index, paddr_t pa);
void commit_dram_cached_page(struct pmobject *pmo, u64 index, paddr_t pa);
void clear_dram_cached_page(struct pmobject *pmo, u64 index);
void clear_dram_cache(struct pmobject *pmo);

paddr_t get_page_from_pmo(struct pmobject *pmo, u64 index);

struct vmregion *init_heap_vmr(struct vmspace *vmspace, vaddr_t va,
                               struct pmobject *pmo);
void adjust_heap_vmr(struct vmspace *vmspace, unsigned long len);

void kprint_vmr(struct vmspace *vmspace);

/* Fork */
int vmspace_clone(struct vmspace *dst_vmspace, struct vmspace *src_vmspace,
                  struct cap_group *dst_cap_group);
int pmo_clone(struct pmobject *dst_pmo, struct pmobject *src_pmo, bool *is_cow);

bool use_radix(struct pmobject *pmo);
bool use_continuous_pages(struct pmobject *pmo);
bool is_external_sync_pmo(struct pmobject *pmo);
bool is_shared_pmo(struct pmobject *pmo);

#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
/* TreeSLS: pmo patch */
void pmo_set_preserved(struct pmobject *pmo);

/* init patch pool */
void *create_patch_pool(void);
#endif
