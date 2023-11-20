// reference: cxl 3.0 sepcification 2022 Aug
#pragma once
#include <common/types.h>

#include "acpi.h"

#define ACPI_CEDT_CFMWS_ARITHMETIC_MODULO   (0)

struct cedt_t {
    struct acpi_sdt_header h;
	u32 entries[];
} __attribute__((__packed__));


// CXL Early Discovery Table
enum CEDT_ENTRY_TYPE {
  ACPI_CXL_HOST_BRIDGE_STRUCTURE = 0,
	ACPI_CXL_FIXED_MEMORY_WINDOW_STRUCTURE = 1,
	ACPI_CXL_XOR_INTERLEAVE_MATH_STRUCTURE = 2,
	ACPI_RDPAS = 3,
	ACPI_CEDT_TYPE_RESERVED = 4 /* 4 and greater are reserved */
};

struct cedt_entry_header {
	u16 entry_type;
	u16 entry_len;
} __attribute__((__packed__));

// 9.17.1.2 CXL HOST BRIDGE STRUCTURE (CHBS)
// a CXL Host Bridge
struct cedt_chbs {
    u8 type; /* Always 0, other values reserved */
    u8 resv1;
    u16 length; /* Length in bytes (32) */
    u32 uid;    /* CXL Host Bridge Unique ID */
    u32 cxl_ver;
    u32 resv2; 
    /*
     * For CXL 1.1, the base is Downstream Port Root Complex Resource Block; 
     * For CXL 2.0, the base is CXL Host Bridge Component Registers[] 
     */
    u64 base;
    u64 len;             
} __attribute__((__packed__));

// 9.17.1.3 CXL Fixed Memory Window Structure (CFMWS)
// The CFMWS structure describes zero or more Host Physical Address (HPA) windows
// that are associated with each CXL Host Bridge. Each window represents a contiguous
// HPA range that may be interleaved across one or more targets, some of which are CXL
// Host Bridges. Associated with each window are a set of restrictions that govern its
// usage. It is the OSPM’s responsibility to utilize each window for the specified use.
// The HPA ranges described by CFMWS may include addresses that are currently
// assigned to CXL.mem devices. Before assigning HPAs from a fixed-memory window,
// the OSPM must check the current assignments and avoid any conflicts.
// For any given HPA, it shall not be described by more than one CFMWS entry.
struct cedt_cfmws {
    u8 type;
    u8 resv1;
    u16 record_length;
    u32 resv2;
    u64 base_hpa; // base of this HPA range, shall be a 256-MB aligined address
    u64 window_size; // total number of consecultive bytes of HPA, shall be a multiple NIW * 256MB  
    u8 eniw;  /* Encoded Number of Interleave Ways */
    u8 interleave_arithmetic; /* Standard Modulo arithmetic (0) */
    u16 resv3;
    u32 hbig; /* Host Bridge Interleave Granularity */
    u16 window_restriction; /* A bitmap describing the restrictions being placed on the OSPM’s use of the window*/
    u16 qtg_id;
    u32 interleave_targets[]; /* Interleave Target List */            
} __attribute__((__packed__));


// TODO: currently, only support first two types

void parse_cedt(struct cedt_t* cedt);

