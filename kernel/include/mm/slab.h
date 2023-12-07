#pragma once

#include <common/list.h>

/*
 * order range: [SLAB_MIN_ORDER, SLAB_MAX_ORDER]
 * ChCore prepares the slab for each order in the range.
 */
#define SLAB_MIN_ORDER (5)
#define SLAB_MAX_ORDER (11)

/* The size of one slab is 2M. */
#define SIZE_OF_ONE_SLAB (2 * 1024 * 1024)

/* slab_header resides in the beginning of each slab (i.e., occupies the first
 * slot). */
struct slab_header {
        /* The list of free slots, which can be converted to struct
         * slab_slot_list. */
        void *free_list_head;
        /* Partial slab list. */
        struct list_head node;

        int order;
        unsigned short total_free_cnt; /* MAX: 65536 */
        unsigned short current_free_cnt;
};

/* Each free slot in one slab is regarded as slab_slot_list. */
struct slab_slot_list {
        void *next_free;
};

struct slab_pointer {
        struct slab_header *current_slab;
        struct list_head partial_slab_list;
};

/* All interfaces are kernel/mm module internal interfaces. */
void init_slab(void);
void *alloc_in_slab(unsigned long);
void free_in_slab(void *addr);
unsigned long get_free_mem_size_from_slab(void);

/* DRAM slabs */
void init_dram_slab(void);
void *alloc_in_dram_slab(unsigned long);
void free_in_dram_slab(void *addr);

/* CXL slabs */
void init_cxl_slab(void);
void *alloc_in_cxl_slab(unsigned long);
void free_in_cxl_slab(void *addr);

#if TRACK_THREAD_MM == ON
u64 size_to_slab_order(u64 size);
#endif
