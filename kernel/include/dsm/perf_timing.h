#ifndef __DSM_PERF_TIMING_H__
#define __DSM_PERF_TIMING_H__

#define PERF_TIMING_CFORK

#ifdef PERF_TIMING_CFORK
#include <arch/time.h>
enum perf_cfork_type {
    PERF_CFORK_KVS_CKPT = 0,
    PERF_CFORK_PREPARE,
    PERF_CFORK_STOP_ALL_THREADS,
    PERF_CFORK_CKPT,
// a deeper breakdown of PERF_CFORK_CKPT
    PERF_CFORK_CKPT_OBJECTS_OTHER,
    PERF_CFORK_CKPT_OBJECTS_CAP_GROUP,
    PERF_CFORK_CKPT_OBJECTS_THREAD,
// end
    PERF_CFORK_KVS_RESTORE,
    PERF_CFORK_RESTORE,
    PERF_CFORK_START_ALL_THREADS,
    PERF_CFORK_TYPE_NR,
};

static const char *perf_cfork_type_str[PERF_CFORK_TYPE_NR] = {
    [0 ... PERF_CFORK_TYPE_NR - 1] = "",
    [PERF_CFORK_KVS_CKPT] = "PERF_CFORK_KVS_CKPT",
    [PERF_CFORK_KVS_RESTORE] = "PERF_CFORK_KVS_RESTORE",
    [PERF_CFORK_START_ALL_THREADS] = "PERF_CFORK_START_ALL_THREADS",
    [PERF_CFORK_STOP_ALL_THREADS] = "PERF_CFORK_STOP_ALL_THREADS",
    [PERF_CFORK_PREPARE] = "PERF_CFORK_PREPARE",
    [PERF_CFORK_CKPT] = "PERF_CFORK_CKPT",
    [PERF_CFORK_RESTORE] = "PERF_CFORK_RESTORE",
    [PERF_CFORK_CKPT_OBJECTS_OTHER] = "PERF_CFORK_CKPT_OBJECTS_OTHER",
    [PERF_CFORK_CKPT_OBJECTS_CAP_GROUP] = "PERF_CFORK_CKPT_OBJECTS_CAP_GROUP",
    [PERF_CFORK_CKPT_OBJECTS_THREAD] = "PERF_CFORK_CKPT_OBJECTS_THREAD",
};

extern u64 perf_cfork_time[];

static inline u64 perf_timing_get_time(void) {
    return plat_get_mono_time(); // ns
}

static inline void print_perf_cfork_time(void) {
    int i;
    for (i = 0; i < PERF_CFORK_TYPE_NR; i++) {
        printk("perf_cfork_time[%s]: %llu us\n", // double precision
            perf_cfork_type_str[i], 
            perf_cfork_time[i] / 1000);
    }
}
#endif

#endif /* __DSM_PERF_TIMING_H__ */
