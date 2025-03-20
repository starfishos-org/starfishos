#pragma once

#include <ckpt/ckpt_data.h>
#include <common/kprint.h>
#include <object/irq.h>
#ifdef CHCORE_SSI_SLS
#include <ckpt/ckpt-dsm.h>
#endif

#define WHOLE_LEVEL    (1)
#define PART_LEVEL     (2)
#define OBJ_LEVEL      (3)
#define FREE_MEM_LEVEL (4)
#define DETAIL_LEVEL   (5)

#define FLAGS_ALLOC             (1 << 0)
#define FLAGS_CFORK             (1 << 1)
#define FLAGS_TIME_TRAVELING    (1 << 2)
#define FLAGS_COW               (1 << 3)

typedef int (*obj_ckpt_func)(struct ckpt_object *, struct ckpt_object *,
                             struct kvs *, int);

typedef int (*obj_restore_func)(struct object *, struct ckpt_object *,
                                struct kvs *, int);
                                
typedef int (*obj_copy_func)(struct ckpt_object *, struct ckpt_object *,
                             struct kvs *);

int system_current_flip_flag; // used to check if a object is checkpointed
#define cfork_current_flip_flag (1) // used to check if a object is checkpointed

int ssi_ckpt_init(void);
int sls_ckpt_init(void);

int sys_whole_ckpt(u64 ckpt_name, u64 name_len);
int sys_whole_ckpt_for_test(u64 ckpt_name, u64 name_len, u64 log_level);
int sys_whole_restore(u64 ckpt_name, u64 name_len);
int sys_copy_time_traveling_data();

#ifdef CHCORE_SSI_SLS
int sys_cfork_prepare(u64 pname_ptr, u64 pname_len);
int sys_cfork_ckpt(u64 pname_ptr, u64 pname_len);
int sys_cfork_restore(u64 pname_ptr, u64 pname_len);
#endif

void sys_ipi_stop_all();
void sys_ipi_start_all();

/* ckpt function */
#ifdef CHCORE_SSI_SLS
void ckpt_dram_cached_page(struct pmobject *pmo, void *kva, u64 index);
int ckpt_dsm_page(struct pmobject *pmo, void *kva, u64 index);
#endif

#ifdef CHCORE_SLS
int ckpt_nvm_page(struct pmobject *pmo, void *kva, u64 index);
void ckpt_dram_cached_page(struct pmobject *pmo, void *kva, u64 index);
#endif

u64 sys_track_pf_begin();
u64 sys_track_pf_end();

#ifdef OMIT_BENCHMARK
static char *benchmark_name_list[3] = {
    "/redis-benchmark",
    "/memcachetest",
    "/ycsbc",
};

static struct vmspace *benchmark_vmspace_list[3];

static inline bool is_benchmark_thread(struct thread *target)
{
    char *name = target->cap_group->cap_group_name;
    for (int i = 0; i < sizeof(benchmark_name_list) / sizeof(benchmark_name_list[0]); i++) {
        if (!strcmp(name, benchmark_name_list[i])) {
            return true;
        }
    }
    return false;
}

static inline bool is_benchmark_vmspace(struct vmspace *vmspace)
{
    for (int i = 0; i < sizeof(benchmark_vmspace_list) / sizeof(benchmark_vmspace_list[0]); i++) {
        if (vmspace == benchmark_vmspace_list[i]) {
            return true;
        }
    }
    return false;
}

static inline bool set_benchmark_vmspace(struct thread *target, struct vmspace *vmspace)
{
    for (int i = 0; i < sizeof(benchmark_vmspace_list) / sizeof(benchmark_vmspace_list[0]); i++) {
        if (benchmark_vmspace_list[i] == NULL) {
            benchmark_vmspace_list[i] = vmspace;
            return true;
        }
    }
    return false;
}
#endif
