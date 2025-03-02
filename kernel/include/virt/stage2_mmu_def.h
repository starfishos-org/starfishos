#pragma once

#include <common/macro.h>

#define MASK(n) (BIT(n) - 1)

#define STAGE2_1G_BLOCK_SHIFT 30
#define STAGE2_2M_BLOCK_SHIFT 21

#define STAGE2_L0_BITS       9
#define STAGE2_L0_ENTRY_BITS 3
#define STAGE2_L0_PTP_BITS   12

#define STAGE2_L1_BITS       9
#define STAGE2_L1_ENTRY_BITS 3
#define STAGE2_L1_PTP_BITS   13

#define STAGE2_L2_BITS       9
#define STAGE2_L2_ENTRY_BITS 3
#define STAGE2_L2_PTP_BITS   12

#define STAGE2_L3_BITS       9
#define STAGE2_L3_ENTRY_BITS 3
#define STAGE2_L3_PTP_BITS   12

#define GET_STAGE2_L0_INDEX(x)                                          \
    (((x) >> (STAGE2_2M_BLOCK_SHIFT + STAGE2_L1_BITS + STAGE2_L2_BITS)) \
     & MASK(STAGE2_L0_BITS))
#define GET_STAGE2_L1_INDEX(x) \
    (((x) >> (STAGE2_2M_BLOCK_SHIFT + STAGE2_L2_BITS)) & MASK(STAGE2_L1_BITS))
#define GET_STAGE2_L2_INDEX(x) \
    (((x) >> (STAGE2_2M_BLOCK_SHIFT)) & MASK(STAGE2_L2_BITS))

/* Secure stage 2 translation output address space */
#define VSTCR_EL2_SA BIT(30)
/* Secure stage 2 translation address space */
#define VSTCR_EL2_SW BIT(31)

/* VTCR_EL2 Registers bits */
#define VTCR_RES1        ((1UL << 31))
#define VTCR_EL2_T0SZ(x) ((64 - (x)))
#define VTCR_SL0_L2      ((0 << 6))
#define VTCR_SL0_L1      ((1 << 6))
#define VTCR_SL0_L0      ((2 << 6))
#define VTCR_IRGN0_WBWC  ((1 << 8))
#define VTCR_IRGN_NC     ((0 << 8))
#define VTCR_IRGN_WBWA   ((1 << 8))
#define VTCR_IRGN_WT     ((2 << 8))
#define VTCR_IRGN_WBnWA  ((3 << 8))
#define VTCR_IRGN_MASK   ((3 << 8))
#define VTCR_ORGN0_WBWC  ((1 << 10))
#define VTCR_ORGN_NC     ((0 << 10))
#define VTCR_ORGN_WBWA   ((1 << 10))
#define VTCR_ORGN_WT     ((2 << 10))
#define VTCR_ORGN_WBnWA  ((3 << 10))
#define VTCR_ORGN_MASK   ((3 << 10))
#define VTCR_SH0_ISH     ((3 << 12))
#define VTCR_TG0_4K      ((0 << 14))
#define VTCR_TG0_64K     ((1 << 14))
#define VTCR_PS_4G       ((0 << 16))
#define VTCR_PS_64G      ((1 << 16))
#define VTCR_PS_1T       ((2 << 16))
#define VTCR_PS_4T       ((3 << 16))
#define VTCR_PS_16T      ((4 << 16))
#define VTCR_PS_256T     ((5 << 16))
#define VTCR_NSA         ((1 << 30))
#define VTCR_NSW         ((1 << 29))

#define UL(x) x##UL

#define VTCR_SH0_SHIFT 12
#define VTCR_SH0_MASK  (UL(3) << VTCR_SH0_SHIFT)
#define VTCR_SH0_INNER (UL(3) << VTCR_SH0_SHIFT)
