#include <common/kvstore.h>
#include <common/util.h>
#include <arch/mmu.h>
#include <mm/mm.h>
#include <mm/kmalloc.h>
#include <mm/uaccess.h>
#include <object/cap_group.h>
#include <object/thread.h>
#include <object/user_fault.h>
#include <ckpt/ckpt.h>
#include <ckpt/ckpt_data.h>
#include <sched/context.h>
#include <dsm/dsm-single.h>
#include <dsm/tiering.h>

#include "cfork.h"

// timing
#ifdef PERF_TIMING_CFORK
u64 perf_cfork_time[PERF_CFORK_TYPE_NR] = {0};
#endif

static inline char *pname_to_pname_ptr(u64 pname_ptr, u64 pname_len)
{
    char *pname = (char *)kmalloc(pname_len + 1, __MT_PRIVATE__);
    copy_from_user(pname, (void *)pname_ptr, pname_len);
    pname[pname_len] = '\0';
    CFORK_LOG_DEBUG("pname: %s, pname_len: %d\n", pname, pname_len);
    return pname;
}

int sys_cfork_prepare(u64 pname_ptr, u64 pname_len)
{
    char *pname;
    int ret = 0;

#ifdef PERF_TIMING_CFORK
    u64 start_time = perf_timing_get_time(), end_time;
#endif
    pname = pname_to_pname_ptr(pname_ptr, pname_len);

    struct cap_group *cap_group;
    struct object *cg_obj;
    struct ckpt_obj_root *cg_obj_root;

    // find the cap group in the cap tree by the process name
    cap_group = find_capgroup_by_name(pname, pname_len);
    if (!cap_group) {
        CFORK_LOG_ERR("cfork_prepare: cap_group not found\n");
        ret = -ENOENT;
        goto out;
    }

    cg_obj = container_of(cap_group, struct object, opaque);

    // checkpoint most memory expect cap_group and thread to the shared memory
    cg_obj_root = cfork_prepare_ckpt_process(cg_obj);
    if (!cg_obj_root) {
        CFORK_LOG_ERR("cfork_prepare: cfork_prepare_ckpt_process failed\n");
        goto out;
    }

    // add the cg_obj_root to the kvs
    ret = add_ckpt_obj_root_by_name(cg_obj_root, pname, pname_len);
    if (ret) {
        CFORK_LOG_ERR("cfork_prepare: add_ckpt_obj_root_by_name failed\n");
        goto out;
    }

    CFORK_LOG_INFO("prepare %s done\n", pname);

out:
    kfree(pname);
#ifdef PERF_TIMING_CFORK
    end_time = plat_get_mono_time();
    perf_cfork_time[PERF_CFORK_KVS_CKPT] += end_time - start_time;
    start_time = end_time;
#endif
    return ret;
}

int sys_cfork_ckpt(u64 pname_ptr, u64 pname_len)
{
#ifdef PERF_TIMING_CFORK
    u64 start_time = plat_get_mono_time(), end_time;
#endif
    char *pname;
    int ret = 0;
    struct ckpt_obj_root *ckpt_obj_root;
    struct cap_group *cap_group;

    pname = pname_to_pname_ptr(pname_ptr, pname_len);

    ckpt_obj_root = find_ckpt_obj_root_by_name(pname, pname_len);
    if (!ckpt_obj_root) {
        CFORK_LOG_ERR("cfork_ckpt: ckpt_obj_root not found\n");
        ret = -ENOENT;
        goto out;
    }

#ifdef PERF_TIMING_CFORK
    end_time = plat_get_mono_time();
    perf_cfork_time[PERF_CFORK_KVS_CKPT] += end_time - start_time;
    start_time = end_time;
#endif
    cap_group = (struct cap_group *)(ckpt_obj_root->obj_src->opaque);

    // remove the process from the cap tree
    ret = stop_all_threads(&(cap_group->thread_list));
    if (ret) {
        CFORK_LOG_ERR("cfork_ckpt: stop_all_threads failed\n");
        goto out;
    }

#ifdef PERF_TIMING_CFORK
    end_time = perf_timing_get_time();
    perf_cfork_time[PERF_CFORK_STOP_ALL_THREADS] += end_time - start_time;
    start_time = end_time;
#endif

    // ret = stop_all_connections(cap_group);
    // if (ret) {
    //     CFORK_LOG_ERR("cfork_ckpt: stop_all_connections failed\n");
    //     goto out;
    // }

    // checkpoint the remaining part to the shared memory
    // ret = cfork_ckpt_process(ckpt_obj_root);
    ret = dsm_migrate_process_prepare(ckpt_obj_root->obj_src);

#ifdef PERF_TIMING_CFORK
    end_time = perf_timing_get_time();
    perf_cfork_time[PERF_CFORK_PREPARE] += end_time - start_time;
    start_time = end_time;
#endif

    ret = dsm_migrate_process_ckpt(ckpt_obj_root->obj_src);

#ifdef PERF_TIMING_CFORK
    end_time = perf_timing_get_time();
    perf_cfork_time[PERF_CFORK_CKPT] += end_time - start_time;
    start_time = end_time;
#endif

    if (ret) {
        CFORK_LOG_ERR("cfork_ckpt: cfork_ckpt_process failed\n");
        ret = -ENOENT;
        goto out;
    }

    ckpt_obj_root->obj_dst = ckpt_obj_root->obj_src->pair_obj;
    ckpt_obj_root->valid = true;
    CFORK_LOG_DEBUG("ckpt_obj_root: %p, ckpt_obj_root->obj_dst: %p\n", ckpt_obj_root, ckpt_obj_root->obj_dst);

    // add the cap group to the kvs
    ret = add_ckpt_obj_root_by_name(ckpt_obj_root, pname, pname_len);
    if (ret) {
        CFORK_LOG_ERR("cfork_ckpt: add_ckpt_obj_root_by_name failed\n");
        goto out;
    }

    CFORK_LOG_INFO("checkpoint %s done\n", pname);

out:
    kfree(pname);
#ifdef PERF_TIMING_CFORK
    end_time = perf_timing_get_time();
    perf_cfork_time[PERF_CFORK_KVS_CKPT] += end_time - start_time;
    start_time = end_time;

    print_perf_cfork_time();
#endif
    return ret;
}

int sys_cfork_restore(u64 pname_ptr, u64 pname_len)
{
#ifdef PERF_TIMING_CFORK
    u64 start_time = perf_timing_get_time(), end_time;
#endif
    int ret = 0, retry_count = 3;
    char *pname;
    struct ckpt_obj_root *ckpt_obj_root;
    struct cap_group *restored_cg;

    pname = pname_to_pname_ptr(pname_ptr, pname_len);

retry:
    // find cap group in the checkpointed cap tree
    if (!(ckpt_obj_root = find_ckpt_obj_root_by_name(pname, pname_len))) {
        CFORK_LOG_ERR("cfork_restore: ckpt_obj_root not found\n");
        ret = -ENOENT;
        goto out;
    }

    if (!ckpt_obj_root->valid) {
        // CFORK_LOG_ERR("cfork_restore: ckpt_obj_root is not valid\n");
        retry_count--;
        if (retry_count > 0) {
            goto retry;
        }
        ret = -ENOENT;
        CFORK_LOG_ERR("cfork_restore: ckpt_obj_root is not valid\n");
        goto out;
    }

    CFORK_LOG_DEBUG("find_ckpt_obj_root_by_name: %p, obj_dst: %p\n", ckpt_obj_root, ckpt_obj_root->obj_dst);

    // restore the cap group
    // if ((ret = cfork_restore_process(ckpt_obj_root, &restored_cg))) {
    //     CFORK_LOG_ERR("cfork_restore: cfork_restore_process failed");
    //     ret = -ENOENT;
    //     goto out;
    // }
    restored_cg = (struct cap_group *)object2obj(ckpt_obj_root->obj_dst);
    CFORK_LOG_DEBUG("restored_cg: %p\n", restored_cg);

#ifdef PERF_TIMING_CFORK
    end_time = perf_timing_get_time();
    perf_cfork_time[PERF_CFORK_KVS_RESTORE] += end_time - start_time;
    start_time = end_time;
#endif


    ret = dsm_migrate_process_restore(restored_cg);
    if (ret) {
        CFORK_LOG_ERR("cfork_restore: dsm_migrate_process_restore failed\n");
        ret = -ENOENT;
        goto out;
    }
    CFORK_LOG_DEBUG("cfork_restore: restored_cg: %p\n", restored_cg);

#ifdef PERF_TIMING_CFORK
    end_time = perf_timing_get_time();
    perf_cfork_time[PERF_CFORK_RESTORE] += end_time - start_time;
    start_time = end_time;
#endif

    // add the restored sub cap group to the cap tree
    if ((ret = add_cap_group_to_cap_tree(root_cap_group, restored_cg))) {
        CFORK_LOG_ERR("cfork_restore: add_cap_group_to_cap_tree failed\n");
        ret = -ENOENT;
        goto out;
    }

    // start all threads
    if ((ret = start_all_threads(&(restored_cg->thread_list)))) {
        CFORK_LOG_ERR("cfork_restore: cfork_start_threads failed\n");
        ret = -ENOENT;
        goto out;
    }
#ifdef PERF_TIMING_CFORK
    end_time = perf_timing_get_time();
    perf_cfork_time[PERF_CFORK_START_ALL_THREADS] += end_time - start_time;
    start_time = end_time;
#endif

    CFORK_LOG_INFO("restore %s done\n", pname);

out:
    kfree(pname);

#ifdef PERF_TIMING_CFORK
    print_perf_cfork_time();
#endif
    return ret;
}
