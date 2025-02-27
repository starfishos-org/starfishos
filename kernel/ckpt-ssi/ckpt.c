#include <object/thread.h>
#include <ckpt/ckpt_data.h>
#include <common/kvstore.h>
#include <common/util.h>
#include <arch/mmu.h>
#include <object/cap_group.h>
#include <object/user_fault.h>
#include <mm/mm.h>
#include <mm/nvm.h>
#include <mm/kmalloc.h>
#include <ckpt/ckpt.h>
#include <sched/context.h>
#include <perf/measure.h>
#include <ckpt/hot_pages_tracker.h>
#include <ckpt/hybird_mem.h>
#include <ckpt/external_sync.h>

#include "ckpt_ws.h"
#include "ckpt_object_pool.h"
#include "ckpt_objects.h"

void flush_tlb_all(void);

int ckpt_metadata_init(void)
{
    int ret = 0;

    /* init ckpt whole system metadata */
    if ((ret = ckpt_ws_init()) != 0)
        goto out_fail;

    /* init ckpt object pool */
    if ((ret = ckpt_obj_map_init()) != 0)
        goto out_fail;

out_fail:
    return ret;
}

#ifdef REPORT
extern u64 patch_page_num;
u64 loop_time = 0;
u64 kvs_get_time = 0;
u64 kvs_put_time = 0;
u64 ckpt_alloc_time = 0;
u64 vmr_ckpt_time = 0;
int lazy_obj_count[TYPE_NR];
u64 track_access_time, track_access_count, track_access_malloc_time;
u64 migrate_pages_time[PLAT_CPU_NUM];
u64 eval_obj_time[TYPE_NR];
u64 get_second_latest_obj_time;
u64 ckpt_vmr_array_reuse_count;
u64 set_write_in_pgtbl_time;
u64 vm_time;
u64 free_time, memcpy_time, radix_time, page_pair_time;
u64 patch_num1, patch_num2;
u64 pool1_time;
u64 init_ckpt_vm_time;
#endif

#ifdef REPORT
int eval_obj_count[TYPE_NR];
#endif

#ifdef REPORT_RUNTIME
u64 pf_count;
u64 pf_tot_time;
#endif

extern int recycle_create_ckpt(struct ckpt_recycle_data *recycle_data);

#ifdef PARALLEL_LOOP
extern bool check_and_adjust;
extern u64 ckpt_max_time;
#endif

/* TODO(MOK): remove the following two variables */
struct ckpt_ws_data *latest_ws_data;
struct ckpt_ws_data *second_latest_ws_data;

struct lock printk_lock;
u64 ckpt_time, running_time, report_loop;
u64 start_time, end_time;
/* TODO: add error code */
u64 track_pf;
u64 *pf_record;
u64 pf_count;
struct vmspace *redis_vmspace;
int sys_whole_ckpt(u64 ckpt_name, u64 name_len)
{
    char *name;
    struct ckpt_ws_data *data;
    struct ckpt_obj_root *root_cap_group_obj_root;
    int r;
    struct ckpt_object *ckpt_obj;
    name = (char *)ckpt_name;

    UNUSED(name);

#ifdef DYN_ADJUST
    DECLTMR2;
    /* check and adjust hotness setting every 1000 ckpt */
    check_and_adjust = (((CKPT_VERSION_NUMBER) & (CHECK_FREQ - 1)) == 0);
    if (unlikely(check_and_adjust))
        start2();
#endif
    /* init ckpt */
    if (unlikely(!CKPT_INITIALIZED))
        CKPT_INITIALIZED = true;
    /* stop all cpus by sending ipis to all remote cpus */
    sys_ipi_stop_all();
#ifdef REPORT_RUNTIME
    printk("[ckpt=%llu] pf_count=%llu, pf_avg_time=%llu\n",
           CKPT_VERSION_NUMBER,
           pf_count,
           pf_count ? pf_tot_time / pf_count : 0);
    pf_count = 0;
    pf_tot_time = 0;
#endif
#ifdef REPORT_HYBRID_MEM
    report_hybrid_mem_and_clear();
#endif
#ifdef EXT_SYNC_ENABLED
    update_external_ringbufs();
#endif
#ifdef HYBRID_MEM
    prepare_process_active_list();
#endif
    data = get_ckpt_ws_data();
    if (!data) {
        r = -ENOMEM;
        goto out_fail;
    }
    set_current_ckpt_version(data->version_number);

    /* flip the flip-flag */
    system_current_flip_flag ^= 1;

    current_thread->thread_ctx->tls_base_reg[TLS_FS] =
            __builtin_ia32_rdfsbase64();
    root_cap_group_obj_root =
            ckpt_obj_root_get(root_cap_group_obj_for_ckpt, true);
    kdebug("whole ckpt obj %d\n", root_cap_group_obj_root->obj->type);
    ckpt_obj = ckpt_obj_get(root_cap_group_obj_root, true);
    BUG_ON(!ckpt_obj);
    data->ckpt_root_obj_root = root_cap_group_obj_root;
    /* TODO(MOK): reuse recycle data and fmap pool */
    recycle_create_ckpt(&data->recycle_data);
    fmap_fault_pool_create_ckpt(&data->ckpt_fmap_fault_pool_list);
#ifdef HYBRID_MEM
#ifdef DYN_ADJUST
    if (unlikely(check_and_adjust))
        ckpt_max_time = stop2();
#endif
    finish_process_active_list();
#endif
    r = ckpt_ws_put(data, (char *)ckpt_name, name_len);
    if (!r) {
        goto out_fail;
    }
    /* TODO(MOK): remove the following two lines*/
    second_latest_ws_data = latest_ws_data;
    latest_ws_data = data;

    smp_mb();

    sys_ipi_start_all();

    flush_tlb_all();

    return 0;
/* TODO: free all we allocate */
out_fail:
    sys_ipi_start_all();
    return r;
}

/* ckpt_name is the buffer start addr, enable_log enable tracking log */
int sys_whole_ckpt_for_test(u64 ckpt_name, u64 name_len, u64 log_level)
{
    struct ckpt_ws_data *data;
    struct ckpt_obj_root *root_cap_group_obj_root;
    int r;
#if defined(WS_PERF) || defined(REPORT)
    memset(eval_obj_count, 0, sizeof eval_obj_count);
    DECLTMR;
    start();
#endif
#ifdef DYN_ADJUST
    DECLTMR2;
    /* check and adjust hotness setting every 1000 ckpt */
    check_and_adjust = (((CKPT_VERSION_NUMBER) & (CHECK_FREQ - 1)) == 0);
    if (unlikely(check_and_adjust))
        start2();
#endif
    /* init ckpt */
    if (unlikely(!CKPT_INITIALIZED))
        CKPT_INITIALIZED = true;
    /* stop all cpus by sending ipis to all remote cpus */
    sys_ipi_stop_all();
#if defined(REPORT)
    u64 ipi_time = plat_get_mono_time() - timer_start;
    memset(migrate_pages_time, 0, sizeof(migrate_pages_time));
#endif
#ifdef HYBRID_MEM
    prepare_process_active_list();
#endif
    data = get_ckpt_ws_data();
    if (!data) {
        r = -ENOMEM;
        goto out_fail;
    }
    set_current_ckpt_version(data->version_number);
    /* flip the flip-flag */
    system_current_flip_flag ^= 1;

#ifdef REPORT
    kvs_get_time = 0;
    kvs_put_time = 0;
    ckpt_alloc_time = 0;
    vmr_ckpt_time = 0;
    memcpy_time = 0;
    free_time = radix_time = page_pair_time = 0;
    patch_num1 = patch_num2 = 0;
    track_access_time = track_access_count = track_access_malloc_time = 0;
    pool1_time = 0;
    init_ckpt_vm_time = 0;
    memset(eval_obj_time, 0, sizeof(eval_obj_time));
    get_second_latest_obj_time = 0;
    set_write_in_pgtbl_time = 0;
    ckpt_vmr_array_reuse_count = 0;
#endif
    current_thread->thread_ctx->tls_base_reg[TLS_FS] =
            __builtin_ia32_rdfsbase64();
    root_cap_group_obj_root =
            ckpt_obj_root_get(root_cap_group_obj_for_ckpt, true);
    BUG_ON(!ckpt_obj_get(root_cap_group_obj_root, true));
    data->ckpt_root_obj_root = root_cap_group_obj_root;
#if defined(REPORT)
    u64 object_time = plat_get_mono_time() - timer_start - ipi_time;
#endif

    recycle_create_ckpt(&data->recycle_data);
#ifdef REPORT
    u64 recycle_time =
            plat_get_mono_time() - timer_start - ipi_time - object_time;
#endif

    init_list_head(&data->ckpt_fmap_fault_pool_list);
    fmap_fault_pool_create_ckpt(&data->ckpt_fmap_fault_pool_list);
#ifdef REPORT
    u64 fmap_time = plat_get_mono_time() - timer_start - recycle_time - ipi_time
                    - object_time;
#endif

#ifdef HYBRID_MEM
#ifdef DYN_ADJUST
    if (unlikely(check_and_adjust))
        ckpt_max_time = stop2();
#endif
    finish_process_active_list();
#endif
#ifdef REPORT
    u64 wait_migrate_finish = plat_get_mono_time() - timer_start - recycle_time
                              - ipi_time - object_time - fmap_time;
#endif
    r = ckpt_ws_put(data, (char *)ckpt_name, name_len);
    if (!r) {
        goto out_fail;
    }

    /* TODO(MOK): remove the following two lines*/
    second_latest_ws_data = latest_ws_data;
    latest_ws_data = data;

#ifdef REPORT
    u64 ckpt_cost_time = stop();
    int i;
    if (loop_time % 1 == 0) {
        DECLTMR;
        start();
        printk("\n==================LOG%d==================\n", loop_time);
        printk("ckpt cost time: %lu\n", ckpt_cost_time);
        printk("ckpt start: %lu\n", timer_start);
        switch (log_level) {
        case FREE_MEM_LEVEL:
            printk("================MEM LEVEL=================\n");
            // printk("before ckpt free mem size: %lu\n", bef_ckpt_free_mem);
            // printk("after ckpt free mem size: %lu\n", get_free_mem_size());
        case DETAIL_LEVEL:
#ifdef DETAIL_REPORT
            printk("================DETAIL LEVEL=================\n");
            // printk("----memcpy time: %lu\n", memcpy_time);
            // printk("----f: %lu, r: %lu, pp: %lu\n",free_time, radix_time,
            // page_pair_time); printk("----patch num1: %lu,
            // num2:%lu\n",patch_num1, patch_num2);
            printk("----init_ckpt_vm_time: %lu\n", init_ckpt_vm_time);
            printk("----vmr: %lu\n", vmr_ckpt_time);
            printk("----vmr array reuse: %lu\n", ckpt_vmr_array_reuse_count);
            printk("----pte pool time: %lu\n", pool1_time);
            printk("--------track access time: %lu, %lu, %lu\n",
                   track_access_count,
                   track_access_malloc_time,
                   track_access_time);
            printk("----kvs get time: %lu\n", kvs_get_time);
            printk("----kvs put time: %lu\n", kvs_put_time);
#endif
        case OBJ_LEVEL:
            printk("================OBJ LEVEL=================\n");
            int tcnt = 0;
            for (i = 0; i < TYPE_NR; i++) {
                printk("object count %d: %d %d, time: %lu\n",
                       i,
                       eval_obj_count[i],
                       lazy_obj_count[i],
                       eval_obj_time[i]);
                tcnt += eval_obj_count[i];
            }
            printk("tcnt: %d\n", tcnt);
        case PART_LEVEL:
            printk("================PART LEVEL================\n");
            printk("ipi time: %lu\n", ipi_time);
            printk("get second latest obj: %lu\n", get_second_latest_obj_time);
            printk("ckpt object time: %lu\n", object_time);
            printk("recycle cost time: %lu\n", recycle_time);
            printk("fmap cost time: %lu\n", fmap_time);
            printk("migrate time: ");
            for (i = 0; i < PLAT_CPU_NUM; i++) {
                printk("cpu[%d]: %lu ", i, migrate_pages_time[i]);
            }
            printk("\n");
            printk("wait for migrate finish time: %lu\n", wait_migrate_finish);
        }
        printk("==================END=====================\n");
    }
    loop_time++;
#endif
#ifdef REPORT_RUNTIME
    printk("pf_count=%llu, pf_avg_time=%llu\n",
           pf_count,
           pf_count ? pf_tot_time / pf_count : 0);
    pf_count = 0;
    pf_tot_time = 0;
#endif
#ifdef REPORT_HYBRID_MEM
    report_hybrid_mem_and_clear();
#endif
#ifdef REPORT
    memset(lazy_obj_count, 0, sizeof lazy_obj_count);
#endif

#ifdef WS_PERF
    printk("\n<wsckpt> %ld %ld\n", , ckpt_cost_time -);
#endif

#ifdef REPORT
    printk("<scnt>");
    int tcnt = 0;
    for (int i = 0; i < TYPE_NR; i++) {
        printk(" %d", eval_obj_count[i]);
        tcnt += eval_obj_count[i];
    }
    printk("\n<tcnt> %d\n", tcnt);
#endif
    smp_mb();

    /* continue all cpus by sending ipis to all remote cpus */
    sys_ipi_start_all();

    void flush_tlb_all(void);
    flush_tlb_all();

    return 0;
/* TODO: free all we allocate */
out_fail:
    sys_ipi_start_all();
    return r;
}

u64 sys_track_pf_begin()
{
    pf_record = kmalloc(sizeof(u64) * 20000000, __SHARED__);
    track_pf = true;

    return 0;
}

u64 sys_track_pf_end()
{
    // sys_ipi_stop_all();
    printk("pf count: %u\n", pf_count);
    printk("===================\n");
    for (int i = 0; i < pf_count; i++) {
        printk("%d: %lx, %lx\n", i, pf_record[i * 2], pf_record[i * 2 + 1]);
    }
    printk("===================\n");
    // sys_ipi_start_all();
    return 0;
}

int sys_ckpt_migrate(u64 ckpt_name)
{
    char *name;
    struct ckpt_object *ckpt_obj;
    struct ckpt_obj_root *root_cap_group_obj_root;
    struct ckpt_ws_data *data;
    int r;

    name = (char *)ckpt_name;
    if (unlikely(!CKPT_INITIALIZED))
        CKPT_INITIALIZED = true;
    /* stop all cpus by sending ipis to all remote cpus */
    sys_ipi_stop_all();

    data = get_ckpt_ws_data();
    if (!data) {
        r = -ENOMEM;
        goto out_fail;
    }

    system_current_flip_flag ^= 1;
    current_thread->thread_ctx->tls_base_reg[TLS_FS] =
            __builtin_ia32_rdfsbase64();
    root_cap_group_obj_root =
            ckpt_obj_root_get(root_cap_group_obj_for_ckpt, true);
    ckpt_obj = ckpt_obj_get(root_cap_group_obj_root, true);
    BUG_ON(!ckpt_obj);
    data->ckpt_root_obj_root = root_cap_group_obj_root;

    recycle_create_ckpt(&data->recycle_data);
    fmap_fault_pool_create_ckpt(&data->ckpt_fmap_fault_pool_list);
#ifdef HYBRID_MEM
#ifdef DYN_ADJUST
    if (unlikely(check_and_adjust))
        ckpt_max_time = stop2();
#endif
    finish_process_active_list();
#endif
    r = ckpt_ws_put(data, (char *)ckpt_name, strlen(name));
    if (!r) {
        goto out_fail;
    }
    /* TODO(MOK): remove the following two lines*/
    second_latest_ws_data = latest_ws_data;
    latest_ws_data = data;

    smp_mb();

    sys_ipi_start_all();

    flush_tlb_all();

    return 0;
out_fail:
    sys_ipi_start_all();
    return r;
}

int sys_ckpt_merge_migration()
{
    struct ckpt_obj_root *root_cap_group_obj_root;
    struct cap_group *cap_group;
    struct ckpt_object *ckpt_obj;
    struct thread *target;
    struct object *thread_obj, *cap_group_obj;
    struct ckpt_thread *ckpt_thread;

    UNUSED(root_cap_group_obj_root);
    UNUSED(cap_group);
    UNUSED(cap_group_obj);
    UNUSED(thread_obj);
    UNUSED(ckpt_obj);
    UNUSED(target);
    UNUSED(ckpt_thread);

    ckpt_obj = (struct ckpt_object *)dsm_dequeue();
    thread_obj = kmalloc(sizeof(struct object), __SHARED__);
    ckpt_thread = (struct ckpt_thread *)ckpt_obj->opaque;
    ckpt_thread->thread_ctx.state = TS_READY;
    thread_restore(thread_obj, ckpt_obj, NULL, false);
    // rr.sched_top();
    // ckpt_obj = (struct ckpt_object *)dsm_dequeue();
    // cap_group_obj = kmalloc(sizeof(struct object), __SHARED__);
    // cap_group_restore(cap_group_obj, ckpt_obj, NULL);
    target = (struct thread *)thread_obj->opaque;
    print_thread(target);

    return 0;
}
