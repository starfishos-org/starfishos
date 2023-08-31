/*
   MIT License

   Copyright (c) 2020 sandwichdoge

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in all
   copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.
*/

/* Reference: https://github.com/sandwichdoge/catchOS/blob/master/kernel/drivers/acpi/madt.h */

#pragma once
#include <common/types.h>

#include "acpi.h"

#define MAX_CPUS_SUPPORTED 128

#define ACPI_MADT_POLARITY_CONFORMS    0
#define ACPI_MADT_POLARITY_ACTIVE_HIGH 1
#define ACPI_MADT_POLARITY_RESERVED    2
#define ACPI_MADT_POLARITY_ACTIVE_LOW  3

#define ACPI_MADT_TRIGGER_CONFORMS (0)
#define ACPI_MADT_TRIGGER_EDGE     (1 << 2)
#define ACPI_MADT_TRIGGER_RESERVED (2 << 2)
#define ACPI_MADT_TRIGGER_LEVEL    (3 << 2)

enum MADT_ENTRY_TYPE {
	MADT_ENTRY_TYPE_LOCAL_APIC = 0,
	MADT_ENTRY_TYPE_IO_APIC = 1,
	MADT_ENTRY_TYPE_SOURCE_OVERRIDE = 2,
	MADT_ENTRY_UNUSED = 3,
	MADT_ENTRY_NMI = 4,
	MADT_ENTRY_TYPE_LOCAL_X2APIC = 9,
	MADT_ENTRY_TYPE_LOCAL_APIC_ADDR_OVERRIDE
};

struct madt_entry_header {
	u8 entry_type;
	/* length include this header + body */
	u8 entry_len;
};

/* Type 0. This type represents a single physical processor and its local interrupt controller. */
struct madt_entry_processor_local_apic {
	struct madt_entry_header h;
	u8 acpi_processor_id;
	u8 apic_id;
    	/* bit 0 = Processor Enabled, bit 1 = Online Capable */
	u32 flags;
};

/* Type 9. This type represents a single physical processor and its local interrupt controller. */
struct madt_entry_processor_local_x2apic {
	struct madt_entry_header h;
	u16 reserved;
	u32 acpi_processor_id;
    	/* bit 0 = Processor Enabled, bit 1 = Online Capable */
	u32 flags;
	u32 apic_id;
};

/* 
 * Type 1. This type represents a I/O APIC. The global system interrupt base is 
 * the first interrupt number that this I/O APIC handles.
 */
struct madt_entry_io_apic {
	struct madt_entry_header h;
	u8 io_apic_id;
	u8 reserved;
	u32 io_apic_addr;
    	/* First first IRQ that this IOAPIC handles */
	u32 global_system_interrupt_base;
};

/* Type 2. This type explains how IRQ sources are mapped to global system interrupts. */
struct madt_interrupt_source_override {
	struct madt_entry_header h;
	u8 bus_source;
	u8 irq_source;
	/* 
	 * Look for I/O APIC with base below this number, then make redirection entry 
	 * (interrupt - base) to be the interrupt.
	 */
	u32 global_system_interrupt;
	u16 flags;
};

/* 
 * Type 4. Configure these with the LINT0 and LINT1 entries in the Local vector table of 
 * the relevant processor(')s(') local APIC. 
 */
struct madt_nonmaskable_interrupts {
	struct madt_entry_header h;
	u8 acpi_processor_id;   // 0xFF means all processors
	u16 flags;
	u8 lint_no;             // 0 or 1
};

/* 
 * Type 5. Provides 64 bit systems with an override of the physical address of the Local APIC.
 * If this structure is defined, the 64-bit Local APIC address stored within it should be used
 * instead of the 32-bit Local APIC address stored in the MADT header
 */
struct madt_local_apic_addr_override {
	struct madt_entry_header h;
	u16 reserved;
	u64 local_apic_phys_addr;
};

struct madt_t {
	struct acpi_sdt_header h;
	u32 local_apic_addr;
    	/* 1 = Dual 8259 Legacy PICs Installed */
	u32 flags;
	u32 entries[];
};

/* Our own struct that contains usable data parsed from MADT. */
struct madt_info_t {
	void *local_apic_addr;
	u8 processor_count;
	u8 processor_ids[MAX_CPUS_SUPPORTED];
	u8 local_apic_ids[MAX_CPUS_SUPPORTED];
	u16 io_apic_count;
	u8 io_apic_ids[32];
	u32 io_apic_addrs[32];
	u32 io_apic_gsi_base[32];
	int irq_override[24];
};

void parse_madt(struct madt_t *madt);
u8 get_cpu_apic_id(u8 cpu_id);
