#include <common/kprint.h>
#include <common/types.h>
#include <drivers/pci.h>
#include <common/list.h>
#include <arch/io.h>

#include "acpi.h"

// TODO: consider merge several parse_table together
void parse_mcfg(struct acpi_table_mcfg *table)
{
    u32 table_len, entry_len;
    struct acpi_mcfg_allocation *entry;
    struct pci_mmcfg_region *cfg;

    // get cedt struct header
    entry = (struct acpi_mcfg_allocation *)GET_TABLE_ENTRY(table);
    table_len = table->header.length;
    entry_len = sizeof(*entry);

    while ((char *)entry + entry_len <= (char *)table + table_len) {
        kinfo("[ACPI] [MCFG INFO] address: 0x%llx, pci_seg: %llx, start_bus_n=%d, end_bus_n=%d\n",
              entry->address,
              entry->pci_segment,
              entry->start_bus_number,
              entry->end_bus_number);
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
