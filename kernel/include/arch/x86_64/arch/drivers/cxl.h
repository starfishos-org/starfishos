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

/* RAS Registers CXL 2.0 8.2.5.9 CXL RAS Capability Structure */
#define CXL_RAS_UNCORRECTABLE_STATUS_OFFSET   0x0
#define CXL_RAS_UNCORRECTABLE_STATUS_MASK     (GENMASK(16, 14) | GENMASK(11, 0))
#define CXL_RAS_UNCORRECTABLE_MASK_OFFSET     0x4
#define CXL_RAS_UNCORRECTABLE_MASK_MASK       (GENMASK(16, 14) | GENMASK(11, 0))
#define CXL_RAS_UNCORRECTABLE_MASK_F256B_MASK BIT(8)
#define CXL_RAS_UNCORRECTABLE_SEVERITY_OFFSET 0x8
#define CXL_RAS_UNCORRECTABLE_SEVERITY_MASK   (GENMASK(16, 14) | GENMASK(11, 0))
#define CXL_RAS_CORRECTABLE_STATUS_OFFSET     0xC
#define CXL_RAS_CORRECTABLE_STATUS_MASK       GENMASK(6, 0)
#define CXL_RAS_CORRECTABLE_MASK_OFFSET       0x10
#define CXL_RAS_CORRECTABLE_MASK_MASK         GENMASK(6, 0)
#define CXL_RAS_CAP_CONTROL_OFFSET            0x14
#define CXL_RAS_CAP_CONTROL_FE_MASK           GENMASK(5, 0)
#define CXL_RAS_HEADER_LOG_OFFSET             0x18
#define CXL_RAS_CAPABILITY_LENGTH             0x58
#define CXL_HEADERLOG_SIZE                    SZ_512
#define CXL_HEADERLOG_SIZE_U32                SZ_512 / sizeof(u32)

/* CXL 2.0 8.2.8.1 Device Capabilities Array Register */
#define CXLDEV_CAP_ARRAY_OFFSET     0x0
#define CXLDEV_CAP_ARRAY_CAP_ID     0
#define CXLDEV_CAP_ARRAY_ID_MASK    GENMASK_ULL(15, 0)
#define CXLDEV_CAP_ARRAY_COUNT_MASK GENMASK_ULL(47, 32)
/* CXL 2.0 8.2.8.2 CXL Device Capability Header Register */
#define CXLDEV_CAP_HDR_CAP_ID_MASK GENMASK(15, 0)
/* CXL 2.0 8.2.8.2.1 CXL Device Capabilities */
#define CXLDEV_CAP_CAP_ID_DEVICE_STATUS     0x1
#define CXLDEV_CAP_CAP_ID_PRIMARY_MAILBOX   0x2
#define CXLDEV_CAP_CAP_ID_SECONDARY_MAILBOX 0x3
#define CXLDEV_CAP_CAP_ID_MEMDEV            0x4000

/* CXL 3.0 8.2.8.3.1 Event Status Register */
#define CXLDEV_DEV_EVENT_STATUS_OFFSET 0x00
#define CXLDEV_EVENT_STATUS_INFO       BIT(0)
#define CXLDEV_EVENT_STATUS_WARN       BIT(1)
#define CXLDEV_EVENT_STATUS_FAIL       BIT(2)
#define CXLDEV_EVENT_STATUS_FATAL      BIT(3)

#define CXLDEV_EVENT_STATUS_ALL                          \
    (CXLDEV_EVENT_STATUS_INFO | CXLDEV_EVENT_STATUS_WARN \
     | CXLDEV_EVENT_STATUS_FAIL | CXLDEV_EVENT_STATUS_FATAL)

/* CXL rev 3.0 section 8.2.9.2.4; Table 8-52 */
#define CXLDEV_EVENT_INT_MODE_MASK   GENMASK(1, 0)
#define CXLDEV_EVENT_INT_MSGNUM_MASK GENMASK(7, 4)

/* CXL 2.0 8.2.8.4 Mailbox Registers */
#define CXLDEV_MBOX_CAPS_OFFSET                0x00
#define CXLDEV_MBOX_CAP_PAYLOAD_SIZE_MASK      GENMASK(4, 0)
#define CXLDEV_MBOX_CAP_BG_CMD_IRQ             BIT(6)
#define CXLDEV_MBOX_CAP_IRQ_MSGNUM_MASK        GENMASK(10, 7)
#define CXLDEV_MBOX_CTRL_OFFSET                0x04
#define CXLDEV_MBOX_CTRL_DOORBELL              BIT(0)
#define CXLDEV_MBOX_CTRL_BG_CMD_IRQ            BIT(2)
#define CXLDEV_MBOX_CMD_OFFSET                 0x08
#define CXLDEV_MBOX_CMD_COMMAND_OPCODE_MASK    GENMASK_ULL(15, 0)
#define CXLDEV_MBOX_CMD_PAYLOAD_LENGTH_MASK    GENMASK_ULL(36, 16)
#define CXLDEV_MBOX_STATUS_OFFSET              0x10
#define CXLDEV_MBOX_STATUS_BG_CMD              BIT(0)
#define CXLDEV_MBOX_STATUS_RET_CODE_MASK       GENMASK_ULL(47, 32)
#define CXLDEV_MBOX_BG_CMD_STATUS_OFFSET       0x18
#define CXLDEV_MBOX_BG_CMD_COMMAND_OPCODE_MASK GENMASK_ULL(15, 0)
#define CXLDEV_MBOX_BG_CMD_COMMAND_PCT_MASK    GENMASK_ULL(22, 16)
#define CXLDEV_MBOX_BG_CMD_COMMAND_RC_MASK     GENMASK_ULL(47, 32)
#define CXLDEV_MBOX_BG_CMD_COMMAND_VENDOR_MASK GENMASK_ULL(63, 48)
#define CXLDEV_MBOX_PAYLOAD_OFFSET             0x20
