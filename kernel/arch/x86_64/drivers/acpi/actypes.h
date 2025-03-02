#pragma once

#include <common/types.h>

/*
 * Miscellaneous types
 */
typedef u32 acpi_status; /* All ACPI Exceptions */
typedef u32 acpi_name; /* 4-byte ACPI name */
typedef char *acpi_string; /* Null terminated ASCII string */
typedef void *acpi_handle; /* Actually a ptr to a NS Node */

/* Time constants for timer calculations */

#define ACPI_MSEC_PER_SEC 1000L

#define ACPI_USEC_PER_MSEC 1000L
#define ACPI_USEC_PER_SEC  1000000L

#define ACPI_100NSEC_PER_USEC 10L
#define ACPI_100NSEC_PER_MSEC 10000L
#define ACPI_100NSEC_PER_SEC  10000000L

#define ACPI_NSEC_PER_USEC 1000L
#define ACPI_NSEC_PER_MSEC 1000000L
#define ACPI_NSEC_PER_SEC  1000000000L

#define ACPI_TIME_AFTER(a, b) ((s64)((b) - (a)) < 0)

/* Owner IDs are used to track namespace nodes for selective deletion */

typedef u16 acpi_owner_id;
#define ACPI_OWNER_ID_MAX 0xFFF /* 4095 possible owner IDs */

#define ACPI_INTEGER_BIT_SIZE     64
#define ACPI_MAX_DECIMAL_DIGITS   20 /* 2^64 = 18,446,744,073,709,551,616 */
#define ACPI_MAX64_DECIMAL_DIGITS 20
#define ACPI_MAX32_DECIMAL_DIGITS 10
#define ACPI_MAX16_DECIMAL_DIGITS 5
#define ACPI_MAX8_DECIMAL_DIGITS  3

/*
 * Value returned by acpi_os_get_thread_id. There is no standard "thread_id"
 * across operating systems or even the various UNIX systems. Since ACPICA
 * only needs the thread ID as a unique thread identifier, we use a u64
 * as the only common data type - it will accommodate any type of pointer or
 * any type of integer. It is up to the host-dependent OSL to cast the
 * native thread ID type to a u64 (in acpi_os_get_thread_id).
 */
#define acpi_thread_id u64

/*******************************************************************************
 *
 * Types specific to 64-bit targets
 *
 ******************************************************************************/

typedef s64 acpi_native_int;

typedef u64 acpi_size;
typedef u64 acpi_io_address;
typedef u64 acpi_physical_address;

#define ACPI_MAX_PTR  ACPI_UINT64_MAX
#define ACPI_SIZE_MAX ACPI_UINT64_MAX

#define ACPI_USE_NATIVE_DIVIDE /* Has native 64-bit integer support */
#define ACPI_USE_NATIVE_MATH64 /* Has native 64-bit integer support */

/******************************************************************************
 *
 * ACPI Specification constants (Do not change unless the specification
 * changes)
 *
 *****************************************************************************/

/* Number of distinct FADT-based GPE register blocks (GPE0 and GPE1) */

#define ACPI_MAX_GPE_BLOCKS 2

/* Default ACPI register widths */

#define ACPI_GPE_REGISTER_WIDTH   8
#define ACPI_PM1_REGISTER_WIDTH   16
#define ACPI_PM2_REGISTER_WIDTH   8
#define ACPI_PM_TIMER_WIDTH       32
#define ACPI_RESET_REGISTER_WIDTH 8

/* Names within the namespace are 4 bytes long */

#define ACPI_NAMESEG_SIZE 4 /* Fixed by ACPI spec */
#define ACPI_PATH_SEGMENT_LENGTH                 \
    5 /* 4 chars for name + 1 char for separator \
       */
#define ACPI_PATH_SEPARATOR '.'

/* Sizes for ACPI table headers */

#define ACPI_OEM_ID_SIZE       6
#define ACPI_OEM_TABLE_ID_SIZE 8

/* ACPI/PNP hardware IDs */

#define PCI_ROOT_HID_STRING         "PNP0A03"
#define PCI_EXPRESS_ROOT_HID_STRING "PNP0A08"
