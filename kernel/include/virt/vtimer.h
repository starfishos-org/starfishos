#pragma once

#include <common/types.h>

struct vtimer {
    /* timer registers for a VCPU */
    u32 cntv_ctl_el0;
    u32 cntv_tval_el0;
    u64 cntvct_el0; /* save-only, used to calculate cntvoff_el2 */
};

void vtimer_init(struct vtimer *);
void vtimer_handle_irq(void);
bool vtimer_is_triggered(struct vtimer *);
void vtimer_save(struct vtimer *);
void vtimer_restore(struct vtimer *);