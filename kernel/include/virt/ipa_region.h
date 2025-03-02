#pragma once
#include <common/list.h>
#include <common/types.h>
#include <virt/vm.h>
#include <virt/stage2_mmu.h>

enum ir_type {
    IR_EAGER_MAPPING = 0,
    IR_LAZY_MAPPING,
};

struct ipa_region {
    struct list_head region_node;

    paddr_t ipa_start;
    /* corresponding pa for this ipa */
    paddr_t pa_start;
    size_t size;
    u32 region_attr; // MMU ATTR for this region
    u32 region_type; // EAGER Mapping or LAZY Mapping?

    /* Which vm this ipa_region belongs to */
    struct virt_vm *vm;
};

struct ipa_region_config_request {
    int pmo_cap;
    paddr_t ipa_start;
    size_t size;
    u32 attr;
};

int add_ipa_region(struct s2mmu *s2mmu, struct ipa_region *region);
struct ipa_region *find_ipa_region_by_ipa(struct s2mmu *s2mmu, paddr_t ipa);
