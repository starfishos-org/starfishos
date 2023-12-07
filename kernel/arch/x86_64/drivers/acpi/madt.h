/*
   MIT License

   Copyright (c) 2020 sandwichdoge

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.
*/

/* Reference:
 * https://github.com/sandwichdoge/catchOS/blob/master/kernel/drivers/acpi/madt.h
 */

#pragma once
#include <common/types.h>

#define MAX_CPUS_SUPPORTED 128

#define ACPI_MADT_POLARITY_CONFORMS    0
#define ACPI_MADT_POLARITY_ACTIVE_HIGH 1
#define ACPI_MADT_POLARITY_RESERVED    2
#define ACPI_MADT_POLARITY_ACTIVE_LOW  3

#define ACPI_MADT_TRIGGER_CONFORMS (0)
#define ACPI_MADT_TRIGGER_EDGE     (1 << 2)
#define ACPI_MADT_TRIGGER_RESERVED (2 << 2)
#define ACPI_MADT_TRIGGER_LEVEL    (3 << 2)

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

u8 get_cpu_apic_id(u8 cpu_id);
