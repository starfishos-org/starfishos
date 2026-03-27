#pragma once

#include <common/types.h>
#include <mm/llfree/llfree_types.h>

#define ll_unused __attribute__((unused))

#ifdef __clang__
#define ll_warn_unused __attribute__((warn_unused_result))
#else
#define ll_warn_unused
#endif

typedef struct ll_optional {
	bool present : 1;
	size_t value : (sizeof(size_t) * 8) - 1;
} ll_optional_t;

static inline ll_unused ll_optional_t ll_some(size_t value)
{
	return (ll_optional_t){ .present = true, .value = value };
}

static inline ll_unused ll_optional_t ll_none(void)
{
	return (ll_optional_t){ .present = false, .value = 0 };
}

typedef struct llfree llfree_t;

enum {
	LLFREE_ERR_OK = 0,
	LLFREE_ERR_MEMORY = 1,
	LLFREE_ERR_RETRY = 2,
	LLFREE_ERR_ADDRESS = 3,
	LLFREE_ERR_INIT = 4,
};

typedef struct ll_warn_unused llfree_result {
	uint64_t frame : (sizeof(uint64_t) * 8) - LLFREE_TIER_BITS - 3;
	uint8_t tier : LLFREE_TIER_BITS;
	uint8_t error : 3;
} llfree_result_t;
_Static_assert(sizeof(llfree_result_t) == sizeof(uint64_t), "result size");

static inline llfree_result_t ll_unused llfree_ok(uint64_t frame, uint8_t tier)
{
	return (llfree_result_t){ .frame = frame, .tier = tier, .error = LLFREE_ERR_OK };
}

static inline llfree_result_t ll_unused llfree_err(uint8_t err)
{
	return (llfree_result_t){ .frame = 0, .tier = 0, .error = err };
}

static inline bool ll_unused llfree_is_ok(llfree_result_t r)
{
	return r.error == LLFREE_ERR_OK;
}

enum {
	LLFREE_INIT_FREE = 0,
	LLFREE_INIT_ALLOC = 1,
	LLFREE_INIT_RECOVER = 2,
	LLFREE_INIT_NONE = 4,
	LLFREE_INIT_MAX = 5,
};

#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)-1)
#endif

#define LLFREE_LOCAL_NONE SIZE_MAX

typedef struct llfree_request {
	uint8_t order;
	uint8_t tier;
	size_t local;
} llfree_request_t;

typedef struct llfree_tree_match {
	ll_optional_t id;
	uint8_t tier;
	size_t free;
} llfree_tree_match_t;

static inline llfree_request_t ll_unused llreq(uint8_t order, uint8_t tier, size_t local)
{
	return (llfree_request_t){ .order = order, .tier = tier, .local = local };
}

typedef enum {
	LLFREE_POLICY_MATCH = 0,
	LLFREE_POLICY_STEAL = 1,
	LLFREE_POLICY_DEMOTE = 2,
	LLFREE_POLICY_INVALID = 3,
} llfree_policy_type_t;

typedef struct {
	llfree_policy_type_t type;
	uint8_t priority;
} llfree_policy_t;

typedef llfree_policy_t (*llfree_policy_fn)(uint8_t requested, uint8_t target, size_t free);

typedef struct llfree_tier_conf {
	uint8_t tier;
	size_t count;
} llfree_tier_conf_t;

typedef struct llfree_tiering {
	llfree_tier_conf_t tiers[LLFREE_MAX_TIERS];
	size_t num_tiers;
	uint8_t default_tier;
	llfree_policy_fn policy;
} llfree_tiering_t;

typedef struct llfree_meta_size {
	size_t llfree;
	size_t local;
	size_t trees;
	size_t lower;
} llfree_meta_size_t;

typedef struct llfree_meta {
	uint8_t *local;
	uint8_t *trees;
	uint8_t *lower;
} llfree_meta_t;

llfree_meta_size_t llfree_metadata_size(const llfree_tiering_t *tiering, size_t frames);
llfree_result_t llfree_init(llfree_t *self, size_t frames, uint8_t init,
			    llfree_meta_t meta, const llfree_tiering_t *tiering);
llfree_result_t llfree_get(llfree_t *self, ll_optional_t frame, llfree_request_t request);
llfree_result_t llfree_put(llfree_t *self, uint64_t frame, llfree_request_t request);
void llfree_drain(llfree_t *self);
size_t llfree_frames(const llfree_t *self);

/* Simple 2-tier policy helpers */
static inline llfree_policy_t ll_unused llfree_simple_policy(uint8_t requested, uint8_t target,
							     size_t free)
{
	if (requested > target)
		return (llfree_policy_t){ LLFREE_POLICY_STEAL, 0 };
	if (requested < target)
		return (llfree_policy_t){ LLFREE_POLICY_DEMOTE, 0 };
	if (free >= LLFREE_TREE_SIZE / 2)
		return (llfree_policy_t){ LLFREE_POLICY_MATCH, 1 };
	if (free >= LLFREE_TREE_SIZE / 64)
		return (llfree_policy_t){ LLFREE_POLICY_MATCH, UINT8_MAX };
	return (llfree_policy_t){ LLFREE_POLICY_MATCH, 0 };
}

static inline llfree_tiering_t ll_unused llfree_tiering_simple(size_t cores)
{
	llfree_tiering_t t = { .num_tiers = 2, .default_tier = 1, .policy = llfree_simple_policy };
	t.tiers[0] = (llfree_tier_conf_t){ .tier = 0, .count = cores };
	t.tiers[1] = (llfree_tier_conf_t){ .tier = 1, .count = cores };
	return t;
}

static inline llfree_request_t ll_unused llfree_simple_request(size_t cores, uint8_t order,
							       size_t core)
{
	if (order >= LLFREE_HUGE_ORDER)
		return llreq(order, 1, core % cores);
	return llreq(order, 0, core % cores);
}

