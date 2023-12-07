#include <common/util.h>
#include <common/types.h>
#include <mm/nvm.h>

#include "acpi.h"

extern struct nvm_region nvm_region_head[8];
extern int nvm_region_num;

void parse_nfit(struct acpi_table_nfit *table)
{
        u32 table_len;
        struct acpi_nfit_header *entry;

        // get cedt struct header
        entry = (struct acpi_nfit_header *)GET_TABLE_ENTRY(table);
        table_len = table->header.length;

        while ((char *)entry + entry->length <= (char *)table + table_len) {
                kdebug("[NFIT INFO] entry->entry_type=%d, entry->entry_len=%d\n",
                       entry->type,
                       entry->length);
                if (entry->length == 0)
                        break;

                switch (entry->type) {
                case ACPI_NFIT_TYPE_SYSTEM_ADDRESS: {
                        struct acpi_nfit_system_address *sys_addr =
                                (struct acpi_nfit_system_address *)entry;
                        kinfo("[NFIT INFO] [NVM] address: 0x%llx, size: %llx, prox_domain=%d\n",
                              sys_addr->address,
                              sys_addr->length,
                              sys_addr->proximity_domain);

                        // TODO: store this info where?
                        nvm_region_head[nvm_region_num].base =
                                sys_addr->address;
                        nvm_region_head[nvm_region_num].length =
                                sys_addr->length;

                        nvm_region_num++;
                        break;
                }
                default:
                        kwarn("Type%d SRAT device is currently not supported\n",
                              entry->type);
                }

                entry = (struct acpi_nfit_header *)((char *)entry
                                                    + entry->length);
        }
}
