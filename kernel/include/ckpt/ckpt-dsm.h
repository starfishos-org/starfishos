#ifndef _CKPT_DSM_H
#define _CKPT_DSM_H

#ifdef CHCORE_SSI_SLS

#include <dsm/dsm-single.h>

#define CKPT_INITIALIZED (dsm_meta->ckpt_initialized)
#define CKPT_VERSION_NUMBER (dsm_meta->version_number)
#define CKPT_WS_TABLE (dsm_meta->ckpt_whole_sys_table)
#define CKPT_CG_KVS (dsm_meta->ckpt_cg_kvs)

inline static void dsm_metadata_init(void)
{
    dsm_meta->ckpt_initialized = false;
    dsm_meta->version_number = 0;
    dsm_meta->ckpt_whole_sys_table = NULL;
    // FIXME(FN): should recycle the kvs
    dsm_meta->ckpt_cg_kvs = new_kvs(19, __SHARED__);
}

inline static void dsm_metadata_reset(void)
{
    dsm_meta->ckpt_initialized = false;
    dsm_meta->version_number = 0;
    dsm_meta->ckpt_whole_sys_table = NULL;
    kvs_free(dsm_meta->ckpt_cg_kvs);
}

inline static void dsm_metadata_set_crash_flag(void)
{
    dsm_meta->crash_last_time = 1;
}

inline static void dsm_metadata_reset_crash_flag(void)
{
    dsm_meta->crash_last_time = 0;
}

inline static void dsm_metadata_set_slabs_data(u64 order,
                                               struct slab_pointer *ptr)
{
    dsm_meta->slab_pool[order].current_slab = ptr->current_slab;
    dsm_meta->slab_pool[order].partial_slab_list = ptr->partial_slab_list;
}

inline static void dsm_metadata_get_slabs_data(u64 order,
                                               struct slab_pointer *ptr)
{
    ptr->current_slab = dsm_meta->slab_pool[order].current_slab;
    ptr->partial_slab_list = dsm_meta->slab_pool[order].partial_slab_list;
}

inline static u64 get_current_ckpt_version()
{
    return CKPT_VERSION_NUMBER;
}

inline static void set_current_ckpt_version(u64 version_number)
{
    CKPT_VERSION_NUMBER = version_number;
}

#endif

#endif
