#include <common/util.h>
#include <common/types.h>
#include <mm/nvm.h>

#include "nfit.h"

extern struct nvm_region nvm_region_head[8];
extern int nvm_region_num;

void parse_nfit(struct nfit_t *nfit)
{
	struct nfit_entry_header *nfit_hdr;
	struct acpi_nfit_spa *spa;

	// get nfit struct header
	nfit_hdr = (struct nfit_entry_header *)((u64)nfit +
				sizeof(struct acpi_sdt_header) + sizeof(u32));

	while (1) {
		if (!nfit_hdr || nfit_hdr->type >= ACPI_NFIT_TYPE_RESERVED || nfit_hdr->type < 0)
			break;

		if (nfit_hdr->type == ACPI_NFIT_TYPE_SAP) {
			spa = (struct acpi_nfit_spa *)nfit_hdr;

			kinfo("[ACPI] NVM REGION[%d]: phys_addr_base: %lx, addr_len: %lx.\n",
				nvm_region_num, spa->phys_addr_base, spa->addr_len);
            
            // TODO: store this info where?
			nvm_region_head[nvm_region_num].base = spa->phys_addr_base;
			nvm_region_head[nvm_region_num].length = spa->addr_len;

			nvm_region_num++;
			break; 
		}
		nfit_hdr += nfit_hdr->length;
	}
}