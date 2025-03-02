#include <mm/numa.h>

void add_mem_to_numa_node(u64 base, u64 size, u64 domain_id)
{
#if 0
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
#endif
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

__attribute__((unused)) static void find_numa_mem_with_id(u64 domain_id,
                                                          u64 *start, u64 *size)
{
    int idx;
    for (idx = 0; idx < MAX_NR_NUMAS; idx++) {
        if (numa_nodes[idx].domain_id == domain_id) {
            *start = numa_nodes[idx].base;
            *size = numa_nodes[idx].size;
            return;
        }
    }
    kwarn("[NUMA] [CXL] numa node (%d) not found\n", domain_id);
}

#ifdef DSM_SHM_DEVICE_CXL_NUMA
void real_cxl_numa_mode_setup_mem(u64 *start, u64 *size)
{
    find_numa_mem_with_id(CXL_NUMA_DOMAIN_ID, start, size);
}

void dram_numa_mode_setup_mem(u64 *start, u64 *size)
{
    find_numa_mem_with_id(0, start, size);
}
#endif