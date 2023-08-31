#pragma once

void tst_mutex(void);
void tst_rwlock(void);
void tst_malloc(void);
void tst_sched(void);
#ifdef PLAT_RASPI4
void tst_multi_buddy(void);
#endif