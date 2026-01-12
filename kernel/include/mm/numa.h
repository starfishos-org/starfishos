#pragma once

#include <common/types.h>
#include <common/macro.h>

#define MAX_NR_NUMAS (4)

struct numa_node {
    u64 domain_id;
    /* memory node */
    // TODO: support multiple mem
    u64 base;
    u64 size;
    /* cpu list */
    // TODO
};

u8 numa_node_nr;
struct numa_node numa_nodes[MAX_NR_NUMAS];

void add_mem_to_numa_node(u64 base, u64 size, u64 domain_id);
void find_numa_mem_with_id(u64 domain_id, u64 *start, u64 *size);

#ifdef DSM_SHM_DEVICE_CXL_NUMA
#define CXL_NUMA_DOMAIN_ID (2)
void real_cxl_numa_mode_setup_mem(u64 *start, u64 *size);
void dram_numa_mode_setup_mem(u64 *start, u64 *size);
#endif
