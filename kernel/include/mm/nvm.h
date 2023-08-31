#pragma once

#include <common/util.h>
#include <common/kprint.h>
#include <mm/slab.h>

/*
 * 				 GLOBAL_MEM_OFF           BUDDY_SYS_OFF
 *      				V                      V
 *  struct nvm_metadata   struct phys_mem_pool   buddy system
 */

#define GLOBAL_MEM_OFFSET (0x2000)
#define BUDDY_SYS_OFFSET  (0x4000)

#define NVM_IS_CRASH (nvm_metadata->crash_last_time)
#define CKPT_INITIALIZED (nvm_metadata->ckpt_initialized)
/* MOK: we no longer use global object map. */
#define CKPT_GLOBAL_OBJ_MAP (nvm_metadata->ckpt_global_obj_map)
#define CKPT_WS_TABLE (nvm_metadata->ckpt_whole_sys_table)
#define CKPT_VERSION_NUMBER (nvm_metadata->version_number)

#define SLAB_POOLS_PTR ((struct slab_pointer*)&(nvm_metadata->slabs))


struct nvm_region {
	u64 base;
	u64 length;
	struct nvm_region *next;
};

struct nvm_metadata {
	/* crash_last_time = 1 means unexpected */
	bool crash_last_time;
	/* Checkpoint time stamp */
	u64 version_number;
	/* Is doing ckpt (or else is restore) */
	bool ckpt_initialized;
	/* Checkpoint data */
	struct ckpt_ws_table *ckpt_whole_sys_table;
	struct kvs *ckpt_global_obj_map;
	struct slab_pointer slabs[SLAB_MAX_ORDER + 1];
};

int nvm_region_num;
struct nvm_region nvm_region_head[8];
struct nvm_metadata *nvm_metadata;
int system_current_flip_flag; // used to check if a object is checkpointed

extern struct ckpt_ws_data *latest_ws_data;
extern struct ckpt_ws_data *second_latest_ws_data;

inline static void nvm_metadata_reset(void)
{
	CKPT_GLOBAL_OBJ_MAP = NULL;
	CKPT_WS_TABLE = NULL;
	latest_ws_data = NULL;
	second_latest_ws_data = NULL;
	CKPT_INITIALIZED = false;
	CKPT_VERSION_NUMBER = 0;
}

inline static void nvm_metadata_set_crash_flag(void)
{ 
	nvm_metadata->crash_last_time = 1;
}

inline static void nvm_metadata_reset_crash_flag(void)
{ 
	nvm_metadata->crash_last_time = 0; 
}

inline static void nvm_metadata_set_slabs_data(u64 order, struct slab_pointer *ptr) 
{
	nvm_metadata->slabs[order].current_slab = ptr->current_slab;
	nvm_metadata->slabs[order].partial_slab_list = ptr->partial_slab_list;
}

inline static void nvm_metadata_get_slabs_data(u64 order, struct slab_pointer *ptr) 
{
	ptr->current_slab = nvm_metadata->slabs[order].current_slab;
	ptr->partial_slab_list = nvm_metadata->slabs[order].partial_slab_list;
}

inline static u64 get_current_ckpt_version()
{
    return CKPT_VERSION_NUMBER;
}

inline static void set_current_ckpt_version(u64 version_number)
{
	CKPT_VERSION_NUMBER = version_number;
}