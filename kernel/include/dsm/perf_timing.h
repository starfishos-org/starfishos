#ifndef __DSM_PERF_TIMING_H__
#define __DSM_PERF_TIMING_H__

#define PERF_TIMING_CFORK

#ifdef PERF_TIMING_CFORK
#include <arch/time.h>
enum perf_cfork_type {
    // prepare
    PERF_CFORK_PREPARE = 0,
    PERF_CFORK_PREPARE_KVS,
    // ckpt
    PERF_CFORK_CKPT,
    PERF_CFORK_CKPT_STOP_ALL_THREADS,
    PERF_CFORK_CKPT_THREADS,
    PERF_CFORK_CKPT_CAP_GROUP,
    PERF_CFORK_CKPT_OTHER,
    // restore
    PERF_CFORK_RESTORE,
    PERF_CFORK_RESTORE_KVS,
    PERF_CFORK_RESTORE_PROMOTE_THREADS,
    PERF_CFORK_RESTORE_START_ALL_THREADS,
    PERF_CFORK_TYPE_NR,
};

static const char *perf_cfork_type_str[PERF_CFORK_TYPE_NR] = {
    [0 ... PERF_CFORK_TYPE_NR - 1] = "",
    [PERF_CFORK_PREPARE] = "PREPARE",
    [PERF_CFORK_PREPARE_KVS] = "PREPARE_KVS",
    [PERF_CFORK_CKPT] = "CKPT",
    [PERF_CFORK_CKPT_STOP_ALL_THREADS] = "CKPT_STOP_ALL_THREADS",
    [PERF_CFORK_CKPT_THREADS] = "CKPT_THREADS",
    [PERF_CFORK_CKPT_CAP_GROUP] = "CKPT_CAP_GROUP",
    [PERF_CFORK_CKPT_OTHER] = "CKPT_OTHER",
    [PERF_CFORK_RESTORE] = "RESTORE",
    [PERF_CFORK_RESTORE_KVS] = "RESTORE_KVS",
    [PERF_CFORK_RESTORE_PROMOTE_THREADS] = "RESTORE_PROMOTE_THREADS",
    [PERF_CFORK_RESTORE_START_ALL_THREADS] = "RESTORE_START_ALL_THREADS",
};

extern u64 perf_cfork_time[];

extern u64 perf_dsm_obj_copy_time[TYPE_NR];
extern u64 perf_dsm_obj_count[TYPE_NR];

static inline u64 perf_timing_get_time(void) {
    return plat_get_mono_time(); // ns
}

static inline void print_perf_cfork_time(void) {
    int i;
    for (i = PERF_CFORK_PREPARE; i < PERF_CFORK_RESTORE; i++) {
        printk("perf_cfork_time[%s]: %llu ns\n", // double precision
            perf_cfork_type_str[i], 
            perf_cfork_time[i]);
    }

    for (i = 0; i < TYPE_NR; i++) {
        printk("prepare copy time object: %s, %llu ns, cnt: %llu\n", // double precision
            obj_name_tbl[i], 
            perf_dsm_obj_copy_time[i],
            perf_dsm_obj_count[i]);
    }
}

static inline void print_perf_restore_time(void) {
    int i;
    for (i = PERF_CFORK_RESTORE; i < PERF_CFORK_TYPE_NR; i++) {
        printk("perf_restore_time[%s]: %llu ns\n", // double precision
            perf_cfork_type_str[i], 
            perf_cfork_time[i]);
    }
}
#endif

#endif /* __DSM_PERF_TIMING_H__ */
