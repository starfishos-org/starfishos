#include <common/kprint.h>
#include <common/types.h>
#include <drivers/pci.h>
#include <common/list.h>
#include <arch/io.h>
#include <arch/mmu.h>

#include "acpi.h"

extern void fill_kernel_page_table_range(u64 mem_start, u64 mem_size);

// TODO: consider merge several parse_table together
void parse_mcfg(struct acpi_table_mcfg *table)
{
    u32 table_len, entry_len;
    struct acpi_mcfg_allocation *entry;
    struct pci_mmcfg_region *cfg;
    u64 mcfg_size;

    // get cedt struct header
    entry = (struct acpi_mcfg_allocation *)GET_TABLE_ENTRY(table);
    table_len = table->header.length;
    entry_len = sizeof(*entry);

    while ((char *)entry + entry_len <= (char *)table + table_len) {
        kdebug("[ACPI] [MCFG INFO] address: 0x%llx, pci_seg: %llx, start_bus_n=%d, end_bus_n=%d\n",
              entry->address,
              entry->pci_segment,
              entry->start_bus_number,
              entry->end_bus_number);
        
        /* Map MCFG MMIO region to kernel page table before accessing it */
        /* MCFG region size: (end_bus - start_bus + 1) * 256 devices * 4KB per device */
        mcfg_size = ((u64)(entry->end_bus_number - entry->start_bus_number + 1)) << 20; /* 1MB per bus */
        fill_kernel_page_table_range(entry->address, mcfg_size);
        
        cfg = pci_mmconfig_add(entry->pci_segment,
                               entry->start_bus_number,
                               entry->end_bus_number,
                               entry->address);
        if (!cfg) {
            kwarn("[MACFG] pci mmconfig add failed\n");
        }
        entry = (struct acpi_mcfg_allocation *)((char *)entry + entry_len);
    }
}
