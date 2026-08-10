#include <assert.h>
#include <stdio.h>

#include <dsm/cxl_reclaim_policy.h>

#define STABLE_EPOCHS 2

static void test_never_touched(void)
{
    struct cxl_policy_page_state state = { 0 };

    assert(cxl_policy_observe_epoch(&state, false, STABLE_EPOCHS)
           == CXL_OBSERVATION_ARMED);
    assert(cxl_policy_observe_epoch(&state, false, STABLE_EPOCHS)
           == CXL_OBSERVATION_ONE_EPOCH_COLD);
    assert(cxl_policy_observe_epoch(&state, false, STABLE_EPOCHS)
           == CXL_OBSERVATION_STABLE_COLD);
}

static void test_first_epoch_then_silent(void)
{
    struct cxl_policy_page_state state = { 0 };

    assert(cxl_policy_observe_epoch(&state, false, STABLE_EPOCHS)
           == CXL_OBSERVATION_ARMED);
    assert(cxl_policy_observe_epoch(&state, true, STABLE_EPOCHS)
           == CXL_OBSERVATION_REREFERENCED);
    assert(cxl_policy_observe_epoch(&state, false, STABLE_EPOCHS)
           == CXL_OBSERVATION_ONE_EPOCH_COLD);
    assert(cxl_policy_observe_epoch(&state, false, STABLE_EPOCHS)
           == CXL_OBSERVATION_STABLE_COLD);
}

static void test_continuously_touched(void)
{
    struct cxl_policy_page_state state = { 0 };
    int epoch;

    assert(cxl_policy_observe_epoch(&state, false, STABLE_EPOCHS)
           == CXL_OBSERVATION_ARMED);
    for (epoch = 0; epoch < 8; epoch++) {
        assert(cxl_policy_observe_epoch(&state, true, STABLE_EPOCHS)
               == CXL_OBSERVATION_REREFERENCED);
        assert(state.cold_epochs == 0);
    }
}

static void test_mapping_generation_starts_fresh_epoch(void)
{
    struct cxl_policy_page_state state = {
        .armed = 1,
        .cold_epochs = 1,
    };

    assert(cxl_policy_rebase_mapping_generation(&state, 7, 8));
    assert(state.armed == 0);
    assert(state.cold_epochs == 0);
    assert(cxl_policy_observe_epoch(&state, false, STABLE_EPOCHS)
           == CXL_OBSERVATION_ARMED);
    assert(cxl_policy_observe_epoch(&state, false, STABLE_EPOCHS)
           == CXL_OBSERVATION_ONE_EPOCH_COLD);
    assert(cxl_policy_observe_epoch(&state, false, STABLE_EPOCHS)
           == CXL_OBSERVATION_STABLE_COLD);

    state.armed = 1;
    state.cold_epochs = 1;
    assert(!cxl_policy_rebase_mapping_generation(&state, 8, 8));
    assert(state.armed == 1);
    assert(state.cold_epochs == 1);
}

static void test_unsafe_mapping_exclusion(void)
{
    assert(!cxl_policy_mapping_eligible(PMO_CODE, true, VMR_READ));
    assert(!cxl_policy_mapping_eligible(PMO_ANONYM,
                                        true,
                                        VMR_READ | VMR_EXEC));
    assert(!cxl_policy_mapping_eligible(PMO_RING_BUFFER_RADIX,
                                        true,
                                        VMR_READ | VMR_WRITE));
    assert(cxl_policy_mapping_eligible(PMO_ANONYM,
                                       true,
                                       VMR_READ | VMR_WRITE));
}

int main(void)
{
    test_never_touched();
    test_first_epoch_then_silent();
    test_continuously_touched();
    test_mapping_generation_starts_fresh_epoch();
    test_unsafe_mapping_exclusion();
    puts("cxl reclaim policy tests: PASS");
    return 0;
}
