#pragma once

#if defined(USE_NVM) && defined(USE_DRAM) && defined(HYBRID_MEM)
#define HYBRID_MEM_ENABLED
#endif

#ifdef REPORT_HYBRID_MEM
extern u64 migrate_to_dram_cnt;
extern u64 migrate_to_nvm_cnt;
void report_hybrid_mem_and_clear(void);
#endif

#define MIGRATE_CPU_NUM (PLAT_CPU_NUM - 1)
extern struct list_head active_list[MIGRATE_CPU_NUM];
extern struct lock active_list_lock[MIGRATE_CPU_NUM];
extern u64 active_list_size;

/*
 * process_sub_active_list - process item in active list
 */
void process_sub_active_list(struct list_head *sublist);

/*
 * process_active_list - migrate pages between DRAM and NVM devices
 *
 * DRAM - K hotest pages
 * NVM - other pages
 *
 * migrate_pages is called during ckpt to aviod conflicts with
 * running theads who acess these migrating pages.
 *
 * TODO(FN): now call this in sequence with sys_whole_ckpt
 * should consider how to do this in parallel
 *
 */
void prepare_process_active_list(void);
void finish_process_active_list(void);
