#pragma once

#include <common/types.h>

#define IPI_TLB_SHOOTDOWN  60
#define IPI_RESCHED        61
#define IPI_WAIT_IN_KERNEL 62
#define IPI_RESET_SCHEDULE 63
#define IPI_STOP_RESCHED   64
#define IPI_TLB_SHOOTDOWN_BATCH 65

void plat_send_ipi(u32 cpu, u32 ipi);
