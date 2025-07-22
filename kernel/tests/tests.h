#pragma once

void tst_mutex(void);
void tst_rwlock(void);
void tst_malloc(u32);
void tst_sched(void);
void tst_malloc_latency(bool);
#ifdef PLAT_RASPI4
void tst_multi_buddy(void);
#endif