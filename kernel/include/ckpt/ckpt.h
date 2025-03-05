#pragma once

#include <ckpt/ckpt_data.h>
#include <common/kprint.h>
#include <object/irq.h>

#define WHOLE_LEVEL    (1)
#define PART_LEVEL     (2)
#define OBJ_LEVEL      (3)
#define FREE_MEM_LEVEL (4)
#define DETAIL_LEVEL   (5)

typedef int (*obj_restore_func)(struct object *, struct ckpt_object *,
                                struct kvs *, bool);
typedef int (*obj_copy_func)(struct ckpt_object *, struct ckpt_object *,
                             struct kvs *);

int ckpt_metadata_init(void);

int sys_whole_ckpt(u64 ckpt_name, u64 name_len);
int sys_whole_ckpt_for_test(u64 ckpt_name, u64 name_len, u64 log_level);
int sys_whole_restore(u64 ckpt_name, u64 name_len);
int sys_copy_time_traveling_data();

int sys_cfork_prepare(u64 pname_ptr, u64 pname_len);
int sys_cfork_ckpt(u64 pname_ptr, u64 pname_len);
int sys_cfork_restore(u64 pname_ptr, u64 pname_len);

void sys_ipi_stop_all();
void sys_ipi_start_all();

/* ckpt function */
#ifdef CHCORE_SSI_SLS
int ckpt_dsm_page(struct pmobject *pmo, void *kva, u64 index);
#else
int ckpt_nvm_page(struct pmobject *pmo, void *kva, u64 index);
#endif
void ckpt_dram_cached_page(struct pmobject *pmo, void *kva, u64 index);

u64 sys_track_pf_begin();
u64 sys_track_pf_end();
