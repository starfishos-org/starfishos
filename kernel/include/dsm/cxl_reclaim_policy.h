#pragma once

#include <arch/mmu.h>
#include <common/types.h>
#include <mm/pmo_types.h>

enum cxl_page_observation {
    CXL_OBSERVATION_ARMED = 0,
    CXL_OBSERVATION_REREFERENCED,
    CXL_OBSERVATION_ONE_EPOCH_COLD,
    CXL_OBSERVATION_STABLE_COLD,
};

struct cxl_policy_page_state {
    u8 armed;
    u8 cold_epochs;
};

/*
 * Replacing an alias can discard the Accessed bit with the old PTE.  Cold
 * history therefore cannot cross a mapping-generation boundary: the first
 * observation after the replacement must establish a fresh epoch.
 */
static inline bool cxl_policy_rebase_mapping_generation(
        struct cxl_policy_page_state *state, u64 previous_generation,
        u64 current_generation)
{
    if (previous_generation == current_generation)
        return false;

    state->armed = 0;
    state->cold_epochs = 0;
    return true;
}

/*
 * Policy consumes observations only after the aging mechanism has completed
 * a full epoch.  It has no knowledge of PTEs, TLBs, or migration entries.
 */
static inline enum cxl_page_observation cxl_policy_observe_epoch(
        struct cxl_policy_page_state *state, bool referenced,
        u8 stable_cold_epochs)
{
    if (!state->armed) {
        state->armed = 1;
        state->cold_epochs = 0;
        return CXL_OBSERVATION_ARMED;
    }
    if (referenced) {
        state->cold_epochs = 0;
        return CXL_OBSERVATION_REREFERENCED;
    }
    if (state->cold_epochs < stable_cold_epochs)
        state->cold_epochs++;
    if (state->cold_epochs < stable_cold_epochs)
        return CXL_OBSERVATION_ONE_EPOCH_COLD;
    return CXL_OBSERVATION_STABLE_COLD;
}

/* Safe-by-default allowlist; new PMO semantics require an explicit opt-in. */
static inline bool cxl_policy_mapping_eligible(pmo_type_t type,
                                               bool radix_backed, u64 perm)
{
    if (!radix_backed
        || (perm & (VMR_EXEC | VMR_DEVICE | VMR_NOCACHE)))
        return false;

    switch (type) {
    case PMO_ANONYM:
    case PMO_DATA:
    case PMO_SHM:
    case PMO_STACK:
    case PMO_HEAP:
        return true;
    case PMO_CODE:
    case PMO_FILE:
    case PMO_USER_PAGER:
    case PMO_DEVICE:
    case PMO_DATA_NOCACHE:
    case PMO_FORBID:
    case PMO_RING_BUFFER:
    case PMO_RING_BUFFER_RADIX:
    case PMO_CROSS_SHM:
    default:
        return false;
    }
}
