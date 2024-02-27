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

inline void add_mem_to_numa_node(u64 base, u64 size, u64 domain_id)
{
        int idx;

        for (idx = 0; idx < MAX_NR_NUMAS; idx++) {
                if (numa_nodes[idx].domain_id == domain_id) {
                        /* already exist */
                        BUG_ON((numa_nodes[idx].base || numa_nodes[idx].size));
                        numa_nodes[idx].base = base;
                        numa_nodes[idx].size = size;
                        return;
                }
        }
        /* new numa node */
        numa_nodes[numa_node_nr].base = base;
        numa_nodes[numa_node_nr].size = size;
        numa_nodes[numa_node_nr].domain_id = domain_id;
        numa_node_nr++;
        kinfo("[NUMA] add numa node (%d) mem: %llx - %llx\n",
                domain_id,
                base,
                base + size);
}

#ifdef DSM_SHM_DEVICE_CXLSPR
#define CXL_NUMA_DOMAIN_ID (2)
inline void real_cxl_numa_mode_setup_mem(u64 *start, u64 *size)
{
        int idx;
        for (idx = 0; idx < MAX_NR_NUMAS; idx++) {
                if (numa_nodes[idx].domain_id == CXL_NUMA_DOMAIN_ID) {
                        *start = numa_nodes[idx].base;
                        *size = numa_nodes[idx].size;
                        return;
                }
        }
        kwarn("[NUMA] [CXL] numa node not found\n");
}
#endif
