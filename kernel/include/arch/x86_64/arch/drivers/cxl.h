#pragma once

#include <common/bitops.h>
#include <common/bitfield.h>

/**
 * DOC: cxl objects
 *
 * The CXL core objects like ports, decoders, and regions are shared
 * between the subsystem drivers cxl_acpi, cxl_pci, and core drivers
 * (port-driver, region-driver, nvdimm object-drivers... etc).
 */

/* CXL 2.0 8.2.4 CXL Component Register Layout and Definition */
#define CXL_COMPONENT_REG_BLOCK_SIZE SZ_64K

/* CXL 2.0 8.2.5 CXL.cache and CXL.mem Registers*/
#define CXL_CM_OFFSET                         0x1000
#define CXL_CM_CAP_HDR_OFFSET                 0x0
#define CXL_CM_CAP_HDR_ID_MASK                GENMASK(15, 0)
#define CM_CAP_HDR_CAP_ID                     1
#define CXL_CM_CAP_HDR_VERSION_MASK           GENMASK(19, 16)
#define CM_CAP_HDR_CAP_VERSION                1
#define CXL_CM_CAP_HDR_CACHE_MEM_VERSION_MASK GENMASK(23, 20)
#define CM_CAP_HDR_CACHE_MEM_VERSION          1
#define CXL_CM_CAP_HDR_ARRAY_SIZE_MASK        GENMASK(31, 24)
#define CXL_CM_CAP_PTR_MASK                   GENMASK(31, 20)

#define CXL_CM_CAP_CAP_ID_RAS      0x2
#define CXL_CM_CAP_CAP_ID_HDM      0x5
#define CXL_CM_CAP_CAP_HDM_VERSION 1

/* HDM decoders CXL 2.0 8.2.5.12 CXL HDM Decoder Capability Structure */
#define CXL_HDM_DECODER_CAP_OFFSET           0x0
#define CXL_HDM_DECODER_COUNT_MASK           GENMASK(3, 0)
#define CXL_HDM_DECODER_TARGET_COUNT_MASK    GENMASK(7, 4)
#define CXL_HDM_DECODER_INTERLEAVE_11_8      BIT(8)
#define CXL_HDM_DECODER_INTERLEAVE_14_12     BIT(9)
#define CXL_HDM_DECODER_CTRL_OFFSET          0x4
#define CXL_HDM_DECODER_ENABLE               BIT(1)
#define CXL_HDM_DECODER0_BASE_LOW_OFFSET(i)  (0x20 * (i) + 0x10)
#define CXL_HDM_DECODER0_BASE_HIGH_OFFSET(i) (0x20 * (i) + 0x14)
#define CXL_HDM_DECODER0_SIZE_LOW_OFFSET(i)  (0x20 * (i) + 0x18)
#define CXL_HDM_DECODER0_SIZE_HIGH_OFFSET(i) (0x20 * (i) + 0x1c)
#define CXL_HDM_DECODER0_CTRL_OFFSET(i)      (0x20 * (i) + 0x20)
#define CXL_HDM_DECODER0_CTRL_IG_MASK        GENMASK(3, 0)
#define CXL_HDM_DECODER0_CTRL_IW_MASK        GENMASK(7, 4)
#define CXL_HDM_DECODER0_CTRL_LOCK           BIT(8)
#define CXL_HDM_DECODER0_CTRL_COMMIT         BIT(9)
#define CXL_HDM_DECODER0_CTRL_COMMITTED      BIT(10)
#define CXL_HDM_DECODER0_CTRL_COMMIT_ERROR   BIT(11)
#define CXL_HDM_DECODER0_CTRL_HOSTONLY       BIT(12)
#define CXL_HDM_DECODER0_TL_LOW(i)           (0x20 * (i) + 0x24)
#define CXL_HDM_DECODER0_TL_HIGH(i)          (0x20 * (i) + 0x28)
#define CXL_HDM_DECODER0_SKIP_LOW(i)         CXL_HDM_DECODER0_TL_LOW(i)
#define CXL_HDM_DECODER0_SKIP_HIGH(i)        CXL_HDM_DECODER0_TL_HIGH(i)

/* HDM decoder control register constants CXL 3.0 8.2.5.19.7 */
#define CXL_DECODER_MIN_GRANULARITY 256
#define CXL_DECODER_MAX_ENCODED_IG  6

/*
 * cxl_decoder flags that define the type of memory / devices this
 * decoder supports as well as configuration lock status See "CXL 2.0
 * 8.2.5.12.7 CXL HDM Decoder 0 Control Register" for details.
 * Additionally indicate whether decoder settings were autodetected,
 * user customized.
 */
#define CXL_DECODER_F_RAM    BIT(0)
#define CXL_DECODER_F_PMEM   BIT(1)
#define CXL_DECODER_F_TYPE2  BIT(2)
#define CXL_DECODER_F_TYPE3  BIT(3)
#define CXL_DECODER_F_LOCK   BIT(4)
#define CXL_DECODER_F_ENABLE BIT(5)
#define CXL_DECODER_F_MASK   GENMASK(5, 0)

enum cxl_decoder_type {
        CXL_DECODER_DEVMEM = 2,
        CXL_DECODER_HOSTONLYMEM = 3,
};
