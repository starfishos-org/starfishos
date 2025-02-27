#include "acpi.h"
#include "actbl3.h"

#ifdef DSM_ENABLED
#include <dsm/dsm.h>
#endif

#include <mm/numa.h>

// TODO: consider merge several parse_table together
void parse_srat(struct acpi_table_srat *table)
{
    u32 table_len;
    struct acpi_subtable_header *entry;

    // get cedt struct header
    entry = (struct acpi_subtable_header *)GET_TABLE_ENTRY(table);
    table_len = table->header.length;

    while ((char *)entry + entry->length <= (char *)table + table_len) {
        kdebug("[SRAT INFO] entry->entry_type=%d, entry->entry_len=%d\n",
               entry->type,
               entry->length);
        if (entry->length == 0)
            break;

        switch (entry->type) {
        case ACPI_SRAT_TYPE_CPU_AFFINITY: {
            break;
        }
        case ACPI_SRAT_TYPE_MEMORY_AFFINITY: {
            struct acpi_srat_mem_affinity *mem_aff =
                    (struct acpi_srat_mem_affinity *)entry;
            if (mem_aff->base_address && mem_aff->length) {
                kinfo("[SRAT INFO] [MEM AFF] address: 0x%llx, size: %llx, prox_domain=%d\n",
                      mem_aff->base_address,
                      mem_aff->length,
                      mem_aff->proximity_domain);
                add_mem_to_numa_node(mem_aff->base_address,
                                     mem_aff->length,
                                     mem_aff->proximity_domain);
            }
            break;
        }
        case ACPI_SRAT_TYPE_X2APIC_CPU_AFFINITY: {
            break;
        }
        default:
            kwarn("Type%d SRAT device is currently not supported\n",
                  entry->type);
        }

        entry = (struct acpi_subtable_header *)((char *)entry + entry->length);
    }
}
