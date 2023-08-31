#pragma once

#define VMEXIT_SYNC     0
#define VMEXIT_IRQ      1
#define VMEXIT_FIQ      2
#define VMEXIT_ERR      3
#define VMEXIT_HYP_SYNC 4
#define VMEXIT_HYP_IRQ  5
#define VMEXIT_HYP_FIQ  6
#define VMEXIT_HYP_ERR  7

/* HVC command definitions */
#define HVC_VM_TEST_END (0x1)
#define HVC_VM_STUB     (0x30)
