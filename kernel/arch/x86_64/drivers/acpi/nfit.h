#pragma once
#include <common/types.h>

#include "acpi.h"

struct nfit_t {
	struct acpi_sdt_header header;
	u32 reserved;
} __attribute__((__packed__));

enum NFIT_ENTRY_TYPE {
	ACPI_NFIT_TYPE_SAP = 0,
	ACPI_NFIT_TYPE_MEMORY_MAP = 1,
	ACPI_NFIT_TYPE_INTERLEAVE = 2,
	ACPI_NFIT_TYPE_SMBIOS = 3,
	ACPI_NFIT_TYPE_CONTROL_REGION = 4,
	ACPI_NFIT_TYPE_DATA_REGION = 5,
	ACPI_NFIT_TYPE_FLUSH_ADDRESS = 6,
	ACPI_NFIT_TYPE_CAPABILITIES = 7,
	ACPI_NFIT_TYPE_RESERVED = 8 /* 8 and greater are reserved */
};

struct nfit_entry_header {
	u16 type;
	u16 length;
} __attribute__((__packed__));

/* 0: System Physical Address Range Structure */

struct acpi_nfit_spa {
	struct nfit_entry_header header;
	u16 spa_id; // SPA Range Structure Index
	u16 flags;  // Bit [0:1] used, Bits [15:2] reserved
	u32 reserved;
	u32 proximity_domain; // Integer that represents the proximity domain to
			      // which the memory belongs
	char range_typr_id[16];
	u64 phys_addr_base;
	u64 addr_len;
	u64 memory_mapping_attr;
} __attribute__((__packed__));

void parse_nfit(struct nfit_t *);
