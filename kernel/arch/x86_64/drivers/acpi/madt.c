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

/* Reference: https://github.com/sandwichdoge/catchOS/blob/master/kernel/drivers/acpi/madt.c */

#include <common/util.h>

#include "madt.h"

static struct madt_info_t madt_info = {
	0,
	.irq_override = {[0 ... 23] = -1},	// init irq_override as -1
};

/*
 * From wiki.osdev.org/MADT:
 *
 * ACPI MADT (Multiple APIC Description Table).
 *   ACPI (Advanced Configuration and Power Interface).
 *   APIC (Advanced Programmable Interrupt Controller).
 *
 * MADT decribes all of the interrupt controllers in the system.
 * MADT starts with the standard ACPI table header and the signature is "APIC".
 * MADT contains a sequence of varibale length entries which enumerate the
 * interrupt devices on the machine.
 */

void parse_madt(struct madt_t *madt)
{
	u8 src;
	u32 madt_len, mapped;
	struct madt_entry_header *entry;
	struct madt_entry_processor_local_apic *local_apic;
	struct madt_entry_processor_local_x2apic *local_x2apic;
	struct madt_entry_io_apic *io_apic;
	struct madt_interrupt_source_override *source_override;

	BUG_ON(!madt);

	madt_info.local_apic_addr = (void *)(u64)madt->local_apic_addr;
	entry = (struct madt_entry_header *)madt->entries;
	madt_len = madt->h.length;
	kdebug("[MADT INFO] local_apic_addr=%lx, entry=%lx, madt=%lx, madt_len=%d\n",
		 (u64)madt_info.local_apic_addr, (u64)((char *)entry), (u64)((char *)madt), madt_len);

	int real_core_cnt = 0; /* Only use cores on the same NUMA */
	while ((char *)entry + entry->entry_len <= (char *)madt + madt_len) {
		kdebug("[MADT INFO] entry->entry_type=%d, entry->entry_len=%d\n", entry->entry_type, entry->entry_len);
		if (entry->entry_len == 0)
			break;

		switch (entry->entry_type) {
		case MADT_ENTRY_TYPE_LOCAL_APIC: {
			local_apic = (struct madt_entry_processor_local_apic *)entry;
			if (local_apic->apic_id == 0xFF)
				break;
			kinfo("[MADT INFO] [Local APIC] ProcessorID [%u], APIC ID[%u], flags[%u]\n",
			       local_apic->acpi_processor_id,
			       local_apic->apic_id,
			       local_apic->flags);
			if (real_core_cnt % 2 == 0) {
				madt_info.processor_ids[madt_info.processor_count] =
					local_apic->acpi_processor_id;
				madt_info.local_apic_ids[madt_info.processor_count] =
					local_apic->apic_id;
				madt_info.processor_count++;
			}
			real_core_cnt++;
			break;
		}

		case MADT_ENTRY_TYPE_LOCAL_X2APIC: {
			local_x2apic = (struct madt_entry_processor_local_x2apic *)entry;
			if (local_x2apic->apic_id == 0xFF)
				break;
			kinfo("[MADT INFO] [Local APIC] ProcessorID [%u], APIC ID[%u], flags[%u]\n",
			       local_x2apic->acpi_processor_id,
			       local_x2apic->apic_id,
			       local_x2apic->flags);
			if (real_core_cnt % 2 == 0) {
				madt_info.processor_ids[madt_info.processor_count] =
					local_x2apic->acpi_processor_id;
				madt_info.local_apic_ids[madt_info.processor_count] =
					local_x2apic->apic_id;
				madt_info.processor_count++;
			}
			real_core_cnt++;
			break;
		}

		case MADT_ENTRY_TYPE_IO_APIC: {
			io_apic = (struct madt_entry_io_apic *)entry;
			madt_info.io_apic_addrs[madt_info.io_apic_count] =
				io_apic->io_apic_addr;
			madt_info.io_apic_ids[madt_info.io_apic_count] =
				io_apic->io_apic_id;
			madt_info.io_apic_gsi_base[madt_info.io_apic_count] =
				io_apic->global_system_interrupt_base;
			madt_info.io_apic_count++;
			break;
		}

		case MADT_ENTRY_TYPE_SOURCE_OVERRIDE: {
			source_override = (struct madt_interrupt_source_override *)entry;
			src = source_override->irq_source;
			mapped = source_override->global_system_interrupt;
			if (src != mapped)
				madt_info.irq_override[src] = mapped;
			break;
		}

		default:
			break;
		}
		entry = (struct madt_entry_header *)((char *)entry
						    + entry->entry_len);
	}
}

u8 get_cpu_apic_id(u8 cpu_id)
{
	BUG_ON(cpu_id < 0 || cpu_id >= madt_info.processor_count);
	return madt_info.local_apic_ids[cpu_id];
}
