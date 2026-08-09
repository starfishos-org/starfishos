#include <common/types.h>
#include <common/macro.h>
#include <common/kprint.h>
#include <common/lock.h>
#include <arch/sync.h>
#include <mm/mm.h>
#include <mm/buddy.h>
#include <mm/kmalloc.h>
#include <mm/remote_free.h>

#ifdef DSM_ENABLED
#include <dsm/dsm-single.h>

/*
 * Cross-machine page free.
 *
 * The pages themselves are unreachable from here: each machine's
 * global_dram_mem[] describes only its own physmem slice, so virt_to_page()
 * on another machine's address finds no pool at all.  Only their physical
 * addresses travel.  They are batched into nodes allocated from CXL, which
 * both machines can read, and pushed onto the owner's stack in dsm_meta;
 * the owner drains the stack from its own allocation path, so every buddy
 * mutation still happens under the owner's local locks.
 *
 * This is the page-level twin of cxl_slab_remote_free (kernel/mm/cxl/
 * cxl_slab.c), which can be simpler because a CXL slot is addressable from
 * every machine and can hold its own list link.
 */

/*
 * Batches still filling up, one per owner.  They are published on flush, so
 * a producer must flush before it can consider a page handed over.
 */
static struct lock rpf_lock;
static struct dram_page_free_batch *rpf_pending[CLUSTER_MAX_MACHINE_NUM];

/*
 * True while any slot of rpf_pending[] is occupied.  Read without the lock so
 * that the common "nothing to publish" case on the allocation path costs a
 * load instead of a contended lock acquisition; a stale read only delays a
 * flush by one poll interval.
 */
static volatile bool rpf_has_pending;

/* Serializes drains and, more importantly, keeps them from nesting. */
static volatile s64 rpf_draining;

/*
 * Set once remote_page_free_init() has cleared this machine's stack.  Until
 * then the stack may still hold addresses queued before this boot, which no
 * longer name live pages; draining them would free them a second time into a
 * freshly initialized buddy.  get_dram_pages() polls from very early on, so
 * the gate matters: dsm_meta becomes usable inside ext_mm_init(), which runs
 * before init and allocates DRAM.
 */
static volatile bool rpf_ready;

/* Machine-local statistics. */
static u64 rpf_queued;
static u64 rpf_drained;
static u64 rpf_dropped;
static u64 rpf_alloc_ticks;

/* Report progress once per this many pages reclaimed (32 MiB). */
#define RPF_REPORT_INTERVAL (8192)

/* Drain at most once per this many local DRAM page allocations. */
#define RPF_POLL_INTERVAL (64)

/*
 * Stack head encoding: (generation << 40) | byte offset from dsm_meta.
 *
 * A plain pointer would be ABA-prone: the owner frees drained nodes back into
 * the CXL slab that every producer allocates from, so the same address can
 * leave the stack and come back while a producer still holds an old snapshot.
 * Bumping the generation on every successful store makes that producer's CAS
 * fail.  dsm_meta sits at the start of the 64-GiB CXL SHM and the nodes are
 * allocated from it, so 40 offset bits still leave an order of magnitude of
 * address-space headroom.  Keeping 48 offset bits left only a 16-bit
 * generation: a migration-heavy run plus the concurrent process teardown can
 * exceed 65,536 push/pop updates for one owner and make the tag itself ABA.
 * The 24-bit generation below raises that bound to over 16 million updates.
 * Offset 0 is dsm_meta itself and therefore remains the empty marker.
 */
#define RPF_OFF_BITS (40)
#define RPF_OFF_MASK ((1UL << RPF_OFF_BITS) - 1)
#define RPF_GEN_BITS (64 - RPF_OFF_BITS)
#define RPF_GEN_MASK ((1UL << RPF_GEN_BITS) - 1)

static u64 rpf_encode(u64 generation, struct dram_page_free_batch *batch)
{
    u64 off = (u64)batch - (u64)dsm_meta;

    BUG_ON(off == 0 || off > RPF_OFF_MASK);
    return ((generation & RPF_GEN_MASK) << RPF_OFF_BITS) | off;
}

static struct dram_page_free_batch *rpf_decode(u64 head)
{
    u64 off = head & RPF_OFF_MASK;

    if (off == 0)
        return NULL;
    return (struct dram_page_free_batch *)((u64)dsm_meta + off);
}

static u64 rpf_next_generation(u64 head)
{
    return (head >> RPF_OFF_BITS) + 1;
}

static void rpf_push(u32 owner, struct dram_page_free_batch *batch)
{
    volatile u64 *headp = &dsm_meta->dram_page_remote_free[owner].head;
    u64 old, desired;

    do {
        old = *headp;
        batch->next = rpf_decode(old);
        desired = rpf_encode(rpf_next_generation(old), batch);
    } while ((u64)compare_and_swap_64((s64 *)headp, (s64)old, (s64)desired)
             != old);
}

static struct dram_page_free_batch *rpf_pop_all(u32 owner)
{
    volatile u64 *headp = &dsm_meta->dram_page_remote_free[owner].head;
    u64 old, desired;

    do {
        old = *headp;
        if ((old & RPF_OFF_MASK) == 0)
            return NULL;
        /* Empty, generation bumped so concurrent pushers notice. */
        desired = (rpf_next_generation(old) & RPF_GEN_MASK) << RPF_OFF_BITS;
    } while ((u64)compare_and_swap_64((s64 *)headp, (s64)old, (s64)desired)
             != old);

    return rpf_decode(old);
}

/* Unconditionally empty the stack without interpreting what was on it. */
static void rpf_discard_all(u32 owner)
{
    volatile u64 *headp = &dsm_meta->dram_page_remote_free[owner].head;
    u64 old, desired;

    do {
        old = *headp;
        desired = (rpf_next_generation(old) & RPF_GEN_MASK) << RPF_OFF_BITS;
    } while ((u64)compare_and_swap_64((s64 *)headp, (s64)old, (s64)desired)
             != old);
}

/* Caller must hold rpf_lock. */
static bool rpf_any_pending(void)
{
    int i;

    for (i = 0; i < CLUSTER_MAX_MACHINE_NUM; i++)
        if (rpf_pending[i] != NULL)
            return true;
    return false;
}

static void remote_page_free(int owner, paddr_t pa)
{
    struct dram_page_free_batch *batch;
    struct dram_page_free_batch *spare = NULL;

    if (owner < 0 || owner >= CLUSTER_MACHINE_NUM || owner == CUR_MACHINE_ID) {
        rpf_dropped++;
        kwarn_once("%s: cannot place pa=0x%lx (owner=%d); leaking it\n",
                   __func__, pa, owner);
        return;
    }

    /*
     * Allocate outside rpf_lock.  The node must live in CXL (the owner has to
     * read it, and this machine's DRAM is as invisible to the owner as its
     * DRAM is to us), but calling into the allocator under rpf_lock would put
     * a whole allocator's worth of code inside this lock's ordering.  The
     * unlocked pre-check can be wrong in both directions, so the loop below
     * closes both: an unneeded node is freed at the end, and a missing one
     * sends us round again rather than dropping the page.
     */
    while (1) {
        if (spare == NULL && rpf_pending[owner] == NULL) {
            spare = kmalloc(sizeof(*spare), __MT_SHARED__);
            if (spare == NULL) {
                rpf_dropped++;
                kwarn_once("%s: out of CXL for a free batch; leaking pa=0x%lx\n",
                           __func__, pa);
                return;
            }
        }

        lock(&rpf_lock);
        batch = rpf_pending[owner];
        if (batch != NULL)
            break;
        if (spare == NULL) {
            /*
             * The slot looked occupied a moment ago and is empty now.  Go
             * back and allocate; the next pass finds either our own node or
             * whatever another CPU installed meanwhile, so this retries at
             * most once.
             */
            unlock(&rpf_lock);
            continue;
        }
        batch = spare;
        spare = NULL;
        batch->next = NULL;
        batch->count = 0;
        batch->owner = (u32)owner;
        rpf_pending[owner] = batch;
        rpf_has_pending = true;
        break;
    }

    batch->pas[batch->count++] = pa;
    rpf_queued++;
    /* Say so once: the counterpart of the reclaim line on the owner. */
    if (rpf_queued == 1)
        kinfo("[DSM] machine %d is handing pages back to their owners "
              "(first: pa=0x%lx owner=%d)\n",
              CUR_MACHINE_ID, pa, owner);

    if (batch->count == DRAM_PAGE_FREE_BATCH_MAX) {
        rpf_pending[owner] = NULL;
        rpf_push((u32)owner, batch);
        rpf_has_pending = rpf_any_pending();
    }
    unlock(&rpf_lock);

    /* Another CPU had already installed a batch for this owner. */
    if (spare != NULL)
        kfree(spare);
}

void remote_page_free_flush(void)
{
    struct dram_page_free_batch *batch;
    int i;

    lock(&rpf_lock);
    for (i = 0; i < CLUSTER_MAX_MACHINE_NUM; i++) {
        batch = rpf_pending[i];
        if (batch == NULL)
            continue;
        rpf_pending[i] = NULL;
        rpf_push((u32)i, batch);
    }
    rpf_has_pending = false;
    unlock(&rpf_lock);
}

bool drain_remote_page_free(void)
{
    struct dram_page_free_batch *batch, *next;
    u64 before = rpf_drained;
    u32 i;

    /*
     * Before init the stack may still hold pre-boot addresses; see rpf_ready.
     */
    if (!rpf_ready)
        return false;
    if ((dsm_meta->dram_page_remote_free[CUR_MACHINE_ID].head & RPF_OFF_MASK)
        == 0)
        return false;

    /*
     * Freeing a batch node is itself a cross-machine free, and freeing a
     * page can return a slab to the buddy, so a drain must not re-enter
     * itself.  A CPU that loses this race simply drains next time.
     */
    if (compare_and_swap_64((s64 *)&rpf_draining, 0, 1) != 0)
        return false;

    batch = rpf_pop_all(CUR_MACHINE_ID);
    while (batch != NULL) {
        next = batch->next;
        for (i = 0; i < batch->count; i++) {
            paddr_t pa = batch->pas[i];

            if (get_paddr_machine_id(pa) != CUR_MACHINE_ID) {
                /*
                 * Not ours: either a stale entry that outlived a reboot or
                 * a corrupted batch.  Dropping it leaks a page; freeing it
                 * would corrupt somebody's buddy state.
                 */
                rpf_dropped++;
                kwarn_once("%s: pa=0x%lx queued to machine %d is not ours\n",
                           __func__, pa, CUR_MACHINE_ID);
                continue;
            }
            kfree((void *)phys_to_virt(pa));
            rpf_drained++;
        }
        kfree(batch);
        batch = next;
    }

    rpf_draining = 0;

    /*
     * Report the first reclaim of this boot, then once per interval: the
     * first one says the mechanism engaged at all, which is what you want
     * when a cross-machine teardown looks like it leaked.
     */
    if (rpf_drained != before
        && (before == 0
            || rpf_drained / RPF_REPORT_INTERVAL
                       != before / RPF_REPORT_INTERVAL))
        kinfo("[DSM] machine %d reclaimed %lu pages freed by other machines\n",
              CUR_MACHINE_ID, rpf_drained);

    return rpf_drained != before;
}

void remote_page_free_poll(void)
{
    /* Racy on purpose: this only decides how often we look at the queue. */
    if (++rpf_alloc_ticks < RPF_POLL_INTERVAL)
        return;
    rpf_alloc_ticks = 0;
    if (!rpf_ready)
        return;
    /*
     * Publish as well as drain.  A partially filled batch is invisible to its
     * owner until it is pushed, and the migration paths queue pages one at a
     * time without ever flushing (only pmo_deinit() does).  Without this, a
     * page freed by migration could sit here until the next PMO teardown, or
     * until reboot if none follows.
     */
    if (rpf_has_pending)
        remote_page_free_flush();
    drain_remote_page_free();
}

void remote_page_free_init(void)
{
    lock_init(&rpf_lock);
    BUG_ON(dsm_meta == NULL);

    /*
     * Drop whatever is queued for us: it predates this boot, our DRAM has
     * just been reinitialized, and those addresses no longer name live
     * pages — the reboot reclaimed them.  Only our own stack is cleared;
     * the other machines' may hold live entries.
     *
     * The chain is dropped without walking it.  A whole-cluster restart
     * reinitializes the CXL allocator underneath these nodes, so following
     * their links would mean freeing stale objects into a fresh slab.  That
     * leaks the nodes when a single machine reboots into a live cluster,
     * which is 2 KiB per 254 pages and bounded by what was in flight.
     *
     * On a cold boot the head may also be uninitialized rather than stale,
     * which is why it is cleared without decoding it.
     */
    rpf_discard_all(CUR_MACHINE_ID);

    /* Only now may anything follow the stack; see rpf_ready. */
    rpf_ready = true;
}

void free_machine_page(paddr_t pa)
{
    int owner = get_paddr_machine_id(pa);

    if (owner != MACHINE_ID_SHARED_MEMORY && owner != CUR_MACHINE_ID) {
        remote_page_free(owner, pa);
        return;
    }
    kfree((void *)phys_to_virt(pa));
}

#else /* !DSM_ENABLED */

void free_machine_page(paddr_t pa)
{
    kfree((void *)phys_to_virt(pa));
}

#endif /* DSM_ENABLED */
