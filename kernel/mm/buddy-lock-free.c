/*
 * Lock-free buddy allocator (C port of linux-tests/lock_free_buddy_allocator).
 * Used for CXL shared phys_mem_pool; metadata (tree + atomic containers) lives in
 * the CXL window so all machines share one allocator via CAS.
 */

#ifdef USE_CXL_MEM

#include <common/kprint.h>
#include <common/macro.h>
#include <common/types.h>
#include <common/util.h> /* memset */
#include <arch/mmu.h>
#include <arch/machine/smp.h>
#include <arch/sync.h>
#include <mm/buddy.h>
#include <mm/mm.h>
#include <mm/rmap.h>

extern struct phys_mem_pool *global_cxl_mem[];
extern int cxlmem_map_num;

extern void destory_track_info(struct page *page);

/* -------------------------------------------------------------------------- */
/* NodeState (state.rs)                                                       */
/* -------------------------------------------------------------------------- */

#define LF_COALESCE_LEFT   ((unsigned long)0x8)
#define LF_COALESCE_RIGHT  ((unsigned long)0x4)
#define LF_LEFT_OCCUPIED   ((unsigned long)0x2)
#define LF_RIGHT_OCCUPIED  ((unsigned long)0x1)

static inline bool lf_is_leaf_pos(u8 pos)
{
	return pos >= 8;
}

static inline unsigned lf_leaf_offset(u8 pos)
{
	BUG_ON(pos < 8);
	return 7 + (5 * (unsigned)(pos - 8));
}

static inline unsigned long lf_state_is_allocable(unsigned long s, u8 pos)
{
	if (pos < 8)
		return (s & (0x1UL << (pos - 1))) == 0;
	return (s & ((0x1FUL << 7) << (5 * (pos - 8)))) == 0;
}

static inline unsigned long lf_lock_not_leaf(unsigned long s, u8 pos)
{
	return s | (0x1UL << (pos - 1));
}

static inline unsigned long lf_lock_leaf(unsigned long s, u8 pos)
{
	return s | (0x13UL << lf_leaf_offset(pos));
}

static inline unsigned long lf_unlock_not_leaf(unsigned long s, u8 pos)
{
	return s & ~(0x1UL << (pos - 1));
}

static inline unsigned long lf_unlock_leaf(unsigned long s, u8 pos)
{
	return s & ~(0x13UL << lf_leaf_offset(pos));
}

static inline unsigned long lf_unlock(unsigned long s, u8 pos)
{
	if (lf_is_leaf_pos(pos))
		return lf_unlock_leaf(s, pos);
	return lf_unlock_not_leaf(s, pos);
}

static inline bool lf_is_occupied(unsigned long s, u8 pos)
{
	if (pos < 8)
		return (s & (0x1UL << (pos - 1))) != 0;
	return (s & ((0x1UL << 6) << (5 * (pos - 7)))) != 0;
}

static inline unsigned long lf_clean_left_coalesce(unsigned long s, u8 pos)
{
	return s & ~(LF_COALESCE_LEFT << lf_leaf_offset(pos));
}

static inline unsigned long lf_clean_rigth_coalesce(unsigned long s, u8 pos)
{
	return s & ~(LF_COALESCE_RIGHT << lf_leaf_offset(pos));
}

static inline unsigned long lf_left_coalesce(unsigned long s, u8 pos)
{
	return s | (LF_COALESCE_LEFT << lf_leaf_offset(pos));
}

static inline unsigned long lf_rigth_coalesce(unsigned long s, u8 pos)
{
	return s | (LF_COALESCE_RIGHT << lf_leaf_offset(pos));
}

static inline unsigned long lf_occupy_left(unsigned long s, u8 pos)
{
	return s | (LF_LEFT_OCCUPIED << lf_leaf_offset(pos));
}

static inline unsigned long lf_occupy_rigth(unsigned long s, u8 pos)
{
	return s | (LF_RIGHT_OCCUPIED << lf_leaf_offset(pos));
}

static inline bool lf_is_left_coalescing(unsigned long s, u8 pos)
{
	return s == lf_left_coalesce(s, pos);
}

static inline bool lf_is_right_coalescing(unsigned long s, u8 pos)
{
	return s == lf_rigth_coalesce(s, pos);
}

static inline unsigned long lf_clean_left_occupy(unsigned long s, u8 pos)
{
	return s & ~(LF_LEFT_OCCUPIED << lf_leaf_offset(pos));
}

static inline unsigned long lf_clean_rigth_occupy(unsigned long s, u8 pos)
{
	return s & ~(LF_RIGHT_OCCUPIED << lf_leaf_offset(pos));
}

static inline bool lf_is_occupied_rigth(unsigned long s, u8 pos)
{
	return s == lf_occupy_rigth(s, pos);
}

static inline bool lf_is_occupied_left(unsigned long s, u8 pos)
{
	return s == lf_occupy_left(s, pos);
}

/* -------------------------------------------------------------------------- */
/* Tree (tree.rs)                                                             */
/* -------------------------------------------------------------------------- */

struct lf_node {
	u16 order_and_pos;
	unsigned long start;
	u32 pos;
	u32 container_offset;
};

struct lf_node_container {
	u64 nodes; /* CAS */
	u32 root;
};

struct buddy_lf_ctx {
	struct phys_mem_pool *pool;
	struct lf_node *tree;
	struct lf_node_container *containers;
	u8 order;
	unsigned long start_page;
};

static struct buddy_lf_ctx buddy_lf_cxl[N_PHYS_MEM_POOLS];
/*
 * Lock-free allocator can livelock under heavy contention or transiently
 * inconsistent shared states. Bound retries to avoid hard hangs.
 */
#define LF_CAS_RETRY_LIMIT 1000000UL
#define LF_ALLOC_SCAN_LIMIT_FACTOR 8UL

static inline u8 lf_node_order(const struct lf_node *n)
{
	return (u8)(n->order_and_pos & 0xff);
}

static inline u8 lf_node_container_pos(const struct lf_node *n)
{
	return (u8)((n->order_and_pos >> 8) + 1);
}

static inline void lf_node_set_order_pos(struct lf_node *n, u8 order, u8 cpos)
{
	/* order can be > 15 on large pools (e.g. 2^20 pages). */
	BUG_ON(cpos == 0 || cpos > 0xf);
	n->order_and_pos = (u16)(order | ((u16)(cpos - 1) << 8));
}

static inline unsigned long lf_num_nodes_from_order(u8 order)
{
	return ((unsigned long)1 << order) * 2 - 1;
}

static inline struct lf_node *lf_node_at(struct buddy_lf_ctx *ctx, u32 p)
{
	return &ctx->tree[p];
}

static inline struct lf_node_container *lf_cont_at(struct buddy_lf_ctx *ctx,
						   u32 off)
{
	return &ctx->containers[off];
}

static inline unsigned long lf_container_get(struct lf_node_container *c)
{
	return (unsigned long)atomic_load_64((s64 *)&c->nodes);
}

static inline bool lf_container_try_update(struct lf_node_container *c,
					   unsigned long oldv,
					   unsigned long newv)
{
	return atomic_bool_compare_exchange_64((s64 *)&c->nodes, (s64)oldv,
						 (s64)newv);
}

static inline u32 lf_parent_pos(u32 pos)
{
	return pos / 2;
}

static inline u32 lf_left_pos(u32 pos)
{
	return pos * 2;
}

static inline u32 lf_right_pos(u32 pos)
{
	return pos * 2 + 1;
}

static inline bool lf_is_leaf(struct buddy_lf_ctx *ctx, const struct lf_node *n)
{
	UNUSED(ctx);
	return lf_node_container_pos(n) >= 8;
}

static inline unsigned lf_height(struct buddy_lf_ctx *ctx)
{
	return (unsigned)ctx->order + 1;
}

static inline unsigned lf_node_count(struct buddy_lf_ctx *ctx)
{
	return (unsigned)lf_num_nodes_from_order(ctx->order);
}

static inline unsigned lf_level(struct buddy_lf_ctx *ctx, const struct lf_node *n)
{
	return lf_height(ctx) - lf_node_order(n);
}

static void lf_init_tree(struct buddy_lf_ctx *ctx)
{
	u8 order = ctx->order;
	unsigned height = (unsigned)order + 1;
	unsigned nodes_count = (unsigned)lf_num_nodes_from_order(order);
	u32 container_num = 0;
	unsigned i;

	for (i = 0; i < nodes_count - 1; ++i) {
		ctx->containers[i].nodes = 0;
		ctx->containers[i].root = 0;
	}

	ctx->tree[1].start = 0;
	ctx->tree[1].pos = 1;
	lf_node_set_order_pos(&ctx->tree[1], order, 1);
	ctx->containers[container_num].root = 1;
	ctx->tree[1].container_offset = container_num;
	container_num++;

	for (i = 2; i <= nodes_count; ++i) {
		u8 child_order = (u8)(lf_node_order(&ctx->tree[i / 2]) - 1);

		ctx->tree[i].pos = (u32)i;

		if ((height - child_order) % 4 == 1) {
			ctx->containers[container_num].root = (u32)i;
			ctx->tree[i].container_offset = container_num;
			container_num++;
			lf_node_set_order_pos(&ctx->tree[i], child_order, 1);
		} else {
			ctx->tree[i].container_offset =
					ctx->tree[i / 2].container_offset;
			if (ctx->tree[i / 2].pos * 2 == (u32)i)
				lf_node_set_order_pos(
						&ctx->tree[i], child_order,
						(u8)(lf_node_container_pos(
								&ctx->tree[i / 2])
						     * 2));
			else
				lf_node_set_order_pos(
						&ctx->tree[i], child_order,
						(u8)(lf_node_container_pos(
								&ctx->tree[i / 2])
						     * 2 + 1));
		}

		if (ctx->tree[i / 2].pos * 2 == (u32)i)
			ctx->tree[i].start = ctx->tree[i / 2].start;
		else
			ctx->tree[i].start =
					ctx->tree[i / 2].start
					+ (1UL << lf_node_order(&ctx->tree[i]));
	}
}

/* Bytes from LF base paddr to end of container array (aligned). */
static size_t lf_reserved_meta_bytes(u8 order)
{
	unsigned long nc = lf_num_nodes_from_order(order);
	size_t tree_b = (size_t)(nc + 1) * sizeof(struct lf_node);
	size_t c_off = ROUND_UP(tree_b, 8);
	size_t cont_b = (size_t)(nc - 1) * sizeof(struct lf_node_container);

	return ROUND_UP(c_off + cont_b, 64);
}

static struct buddy_lf_ctx *lf_ctx_for_pool(struct phys_mem_pool *pool)
{
	int i;

	for (i = 0; i < cxlmem_map_num && i < N_PHYS_MEM_POOLS; ++i) {
		if (buddy_lf_cxl[i].pool == pool)
			return &buddy_lf_cxl[i];
	}
	return NULL;
}

/* forward decl */
static void lf_free_node(struct buddy_lf_ctx *ctx, const struct lf_node *node,
			 const struct lf_node *upper_bound);

static unsigned long lf_lock_descendants(struct buddy_lf_ctx *ctx,
					 const struct lf_node *node,
					 unsigned long val)
{
	u32 p = node->pos;

	if ((unsigned long)p * 2 >= (unsigned long)lf_node_count(ctx))
		return val;

	BUG_ON(lf_node_order(node) == 0);
	BUG_ON(lf_is_leaf(ctx, lf_node_at(ctx, lf_left_pos(p)))
	       != lf_is_leaf(ctx, lf_node_at(ctx, lf_right_pos(p))));

	if (!lf_is_leaf(ctx, lf_node_at(ctx, lf_left_pos(p)))) {
		val = lf_lock_not_leaf(
				val, lf_node_container_pos(lf_node_at(
						     ctx, lf_left_pos(p))));
		val = lf_lock_not_leaf(
				val, lf_node_container_pos(lf_node_at(
						     ctx, lf_right_pos(p))));
		val = lf_lock_descendants(ctx,
					  lf_node_at(ctx, lf_left_pos(p)),
					  val);
		return lf_lock_descendants(ctx,
					   lf_node_at(ctx, lf_right_pos(p)),
					   val);
	}
	val = lf_lock_leaf(val, lf_node_container_pos(lf_node_at(
						     ctx, lf_left_pos(p))));
	return lf_lock_leaf(val, lf_node_container_pos(lf_node_at(
						     ctx, lf_right_pos(p))));
}

/*
 * Mirrors Rust Tree::unlock_descendants (despite the name, it applies
 * lock_not_leaf / lock_leaf to descendant slots in the shared state word).
 */
static unsigned long lf_unlock_descendants(struct buddy_lf_ctx *ctx,
					   const struct lf_node *node,
					   unsigned long val)
{
	u32 p = node->pos;

	if ((unsigned long)p * 2 >= (unsigned long)lf_node_count(ctx))
		return val;

	if (!lf_is_leaf(ctx, lf_node_at(ctx, lf_left_pos(p)))) {
		val = lf_lock_not_leaf(
				val, lf_node_container_pos(lf_node_at(
						     ctx, lf_left_pos(p))));
		val = lf_lock_not_leaf(
				val, lf_node_container_pos(lf_node_at(
						     ctx, lf_right_pos(p))));
		val = lf_unlock_descendants(ctx,
					    lf_node_at(ctx, lf_left_pos(p)),
					    val);
		return lf_unlock_descendants(ctx,
					     lf_node_at(ctx, lf_right_pos(p)),
					     val);
	}
	val = lf_lock_leaf(val, lf_node_container_pos(lf_node_at(
					     ctx, lf_left_pos(p))));
	return lf_lock_leaf(val, lf_node_container_pos(lf_node_at(
					     ctx, lf_right_pos(p))));
}

static bool lf_check_brother(struct buddy_lf_ctx *ctx, const struct lf_node *node,
			     unsigned long val)
{
	const struct lf_node *parent = lf_node_at(ctx, lf_parent_pos(node->pos));
	const struct lf_node *l_parent = lf_node_at(ctx, lf_left_pos(parent->pos));
	const struct lf_node *r_parent = lf_node_at(ctx, lf_right_pos(parent->pos));

	return (l_parent == node
		&& !lf_state_is_allocable(val,
					  lf_node_container_pos(r_parent)))
	       || (r_parent == node
		   && !lf_state_is_allocable(val,
					     lf_node_container_pos(l_parent)));
}

static void lf_unmark(struct buddy_lf_ctx *ctx, const struct lf_node *node,
		      const struct lf_node *upper_bound);

static void lf_mark(struct buddy_lf_ctx *ctx, const struct lf_node *node,
		    const struct lf_node *upper_bound)
{
	const struct lf_node *parent = lf_node_at(ctx, lf_parent_pos(node->pos));
	struct lf_node_container *container =
			lf_cont_at(ctx, parent->container_offset);

	while (1) {
		unsigned long new_val = lf_container_get(container);
		unsigned long old_val = new_val;

		if (lf_node_at(ctx, lf_left_pos(parent->pos)) == node)
			new_val = lf_left_coalesce(new_val,
						   lf_node_container_pos(parent));
		else
			new_val = lf_rigth_coalesce(new_val,
						    lf_node_container_pos(parent));

		if (lf_container_try_update(container, old_val, new_val))
			break;
	}

	{
		const struct lf_node *root =
				lf_node_at(ctx, container->root);
		if (root->pos != upper_bound->pos)
			lf_mark(ctx, root, upper_bound);
	}
}

static void lf_unmark(struct buddy_lf_ctx *ctx, const struct lf_node *node,
		      const struct lf_node *upper_bound)
{
	const struct lf_node *cur;
	bool exit;

	while (1) {
		const struct lf_node *parent =
				lf_node_at(ctx, lf_parent_pos(node->pos));
		struct lf_node_container *container =
				lf_cont_at(ctx, parent->container_offset);
		unsigned long new_val = lf_container_get(container);
		unsigned long old_val = new_val;

		cur = node;
		exit = false;

		if (lf_node_at(ctx, lf_left_pos(parent->pos)) == node) {
			if (!lf_is_left_coalescing(new_val,
						   lf_node_container_pos(parent)))
				return;

			new_val = lf_clean_left_coalesce(
					new_val, lf_node_container_pos(parent));
			new_val = lf_clean_left_occupy(
					new_val, lf_node_container_pos(parent));

			if (lf_is_occupied_rigth(new_val,
						 lf_node_container_pos(parent))) {
				if (lf_container_try_update(container, old_val,
							    new_val))
					break;
				else
					continue;
			}
		}

		if (lf_node_at(ctx, lf_right_pos(parent->pos)) == node) {
			if (!lf_is_right_coalescing(new_val,
						    lf_node_container_pos(parent)))
				return;

			new_val = lf_clean_rigth_coalesce(
					new_val, lf_node_container_pos(parent));
			new_val = lf_clean_rigth_occupy(
					new_val, lf_node_container_pos(parent));

			if (lf_is_occupied_left(new_val,
						lf_node_container_pos(parent))) {
				if (lf_container_try_update(container, old_val,
							    new_val))
					break;
				else
					continue;
			}
		}

		cur = lf_node_at(ctx, lf_parent_pos(node->pos));
		{
			struct lf_node_container *cur_cont =
					lf_cont_at(ctx, cur->container_offset);
			const struct lf_node *cur_root =
					lf_node_at(ctx, cur_cont->root);

			while (cur->pos != cur_root->pos) {
				exit = lf_check_brother(ctx, cur, new_val);
				if (exit)
					break;

				new_val = lf_unlock_not_leaf(
						new_val,
						lf_node_container_pos(lf_node_at(
							      ctx,
							      lf_parent_pos(
								      cur->pos))));
				cur = lf_node_at(ctx, lf_parent_pos(cur->pos));
			}
		}

		if (lf_container_try_update(container, old_val, new_val))
			break;
	}

	if (cur->pos != upper_bound->pos && !exit)
		lf_unmark(ctx, cur, upper_bound);
}

static void lf_free_node(struct buddy_lf_ctx *ctx, const struct lf_node *node,
			 const struct lf_node *upper_bound)
{
	struct lf_node_container *container =
			lf_cont_at(ctx, node->container_offset);
	const struct lf_node *root = lf_node_at(ctx, container->root);
	bool exit;

	if (root->pos != upper_bound->pos)
		lf_mark(ctx, root, upper_bound);

	while (1) {
		unsigned long new_val = lf_container_get(container);
		unsigned long old_val = new_val;
		const struct lf_node *cur = node;

		exit = false;

		while (cur->pos != root->pos) {
			exit = lf_check_brother(ctx, cur, new_val);
			if (exit)
				break;

			new_val = lf_unlock_not_leaf(
					new_val,
					lf_node_container_pos(lf_node_at(
							ctx,
							lf_parent_pos(cur->pos))));
			cur = lf_node_at(ctx, lf_parent_pos(cur->pos));
		}

		if (!lf_is_leaf(ctx, node)
		    && (unsigned long)node->pos * 2
			       <= (unsigned long)lf_node_count(ctx))
			new_val = lf_unlock_descendants(ctx, node, new_val);

		new_val = lf_unlock(new_val, lf_node_container_pos(node));

		if (lf_container_try_update(container, old_val, new_val))
			break;
	}

	root = lf_node_at(ctx, container->root);
	if (root->pos != upper_bound->pos && !exit)
		lf_unmark(ctx, root, upper_bound);
}

/*
 * true  = conflict (rollback): *out_i / *out_n for retry
 * false = no conflict (alloc committed)
 */
static bool lf_check_parent_conflict(struct buddy_lf_ctx *ctx,
				     const struct lf_node *node, u32 *out_i,
				     u32 *out_n)
{
	const struct lf_node *parent = lf_node_at(ctx, lf_parent_pos(node->pos));
	struct lf_node_container *container_parent =
			lf_cont_at(ctx, parent->container_offset);
	const struct lf_node *root = lf_node_at(ctx, container_parent->root);

	while (1) {
		unsigned long new_val = lf_container_get(container_parent);
		unsigned long old_val = new_val;

		if (lf_is_occupied(new_val, lf_node_container_pos(parent))) {
			*out_i = parent->pos;
			*out_n = node->pos;
			return true;
		}

		if (lf_node_at(ctx, lf_left_pos(parent->pos)) == node)
			new_val = lf_occupy_left(
					lf_clean_left_coalesce(
						new_val,
						lf_node_container_pos(parent)),
					lf_node_container_pos(parent));
		else
			new_val = lf_occupy_rigth(
					lf_clean_rigth_coalesce(
						new_val,
						lf_node_container_pos(parent)),
					lf_node_container_pos(parent));

		new_val = lf_lock_not_leaf(
				new_val,
				lf_node_container_pos(lf_node_at(
						ctx, lf_parent_pos(parent->pos))));
		new_val = lf_lock_not_leaf(
				new_val,
				lf_node_container_pos(lf_node_at(
						ctx, lf_parent_pos(parent->pos))));
		new_val = lf_lock_not_leaf(new_val, lf_node_container_pos(root));

		if (!lf_container_try_update(container_parent, old_val, new_val))
			continue;
		break;
	}

	if (root->pos == lf_node_at(ctx, 1)->pos)
		return false;
	return lf_check_parent_conflict(ctx, root, out_i, out_n);
}

/* NULL = allocated OK; non-NULL = retry from that node position */
static const struct lf_node *lf_try_alloc_node(struct buddy_lf_ctx *ctx,
					       const struct lf_node *node)
{
	struct lf_node_container *container =
			lf_cont_at(ctx, node->container_offset);
	u32 i, n;
	unsigned long retries = 0;

	BUG_ON(lf_node_container_pos(node) == 0);

	while (1) {
		unsigned long new_val = lf_container_get(container);
		if (!lf_state_is_allocable(new_val, lf_node_container_pos(node)))
			return lf_node_at(ctx, node->pos);

		{
			unsigned long old_val = new_val;
			u32 root_pos = lf_node_at(ctx, container->root)->pos;
			const struct lf_node *cur = node;

			while (cur->pos != root_pos) {
				new_val = lf_lock_not_leaf(
						new_val,
						lf_node_container_pos(lf_node_at(
							      ctx,
							      lf_parent_pos(
								      cur->pos))));
				cur = lf_node_at(ctx, lf_parent_pos(cur->pos));
			}

			if (lf_is_leaf(ctx, node))
				new_val = lf_lock_leaf(new_val,
						       lf_node_container_pos(
							       node));
			else {
				new_val = lf_lock_not_leaf(
						new_val,
						lf_node_container_pos(node));
				if ((unsigned long)node->pos * 2
				    < (unsigned long)lf_node_count(ctx))
					new_val = lf_lock_descendants(ctx, node,
								      new_val);
			}

			if (lf_container_try_update(container, old_val, new_val))
				break;
		}

		retries++;
		if (unlikely(retries > LF_CAS_RETRY_LIMIT)) {
			kwarn("buddy_lf: alloc CAS livelock at pos=%u root=%u\n",
			      node->pos, container->root);
			return lf_node_at(ctx, 1);
		}
		asm volatile("pause");
	}

	if (lf_node_at(ctx, container->root)->pos
	    == lf_node_at(ctx, 1)->pos)
		return NULL;

	if (!lf_check_parent_conflict(ctx,
				     lf_node_at(ctx, container->root), &i,
				     &n))
		return NULL;

	lf_free_node(ctx, node, lf_node_at(ctx, n));
	return lf_node_at(ctx, i);
}

static bool lf_do_alloc(struct buddy_lf_ctx *ctx, unsigned alloc_order,
			unsigned long *out_page_idx)
{
	unsigned start_node = 1U << (ctx->order - (u8)alloc_order);
	unsigned last_node;
	unsigned long a;
	unsigned started_at;
	bool restarted = false;
	unsigned long scan_budget = 0;
	unsigned long scan_limit;

	if (alloc_order == 0)
		last_node = lf_node_count(ctx);
	else
		last_node = (unsigned)lf_left_pos(lf_node_at(ctx, (u32)start_node)
							  ->pos)
			    - 1;

	a = smp_get_cpu_id();
	if (last_node - start_node != 0)
		a %= (last_node - start_node);
	else
		a = 0;
	a += start_node;
	started_at = a;
	scan_limit = ((unsigned long)(last_node - start_node + 1)
		      * LF_ALLOC_SCAN_LIMIT_FACTOR)
		     + 1;

	for (;;) {
		const struct lf_node *n = lf_node_at(ctx, (u32)a);
		const struct lf_node *r;

		BUG_ON(lf_node_order(n) != alloc_order);

		r = lf_try_alloc_node(ctx, n);
		if (r == NULL) {
			*out_page_idx = ctx->start_page + n->start;
			return true;
		}
		if (r->pos == 1)
			return false;

		a = (r->pos + 1)
		    * (1UL << (lf_level(ctx, n) - lf_level(ctx, r)));

		if (a > last_node) {
			a = start_node;
			restarted = true;
		}

		if (restarted && a >= started_at)
			return false;

		scan_budget++;
		if (unlikely(scan_budget > scan_limit)) {
			kwarn("buddy_lf: alloc scan exceeded (order=%u, start=%u, last=%u)\n",
			      alloc_order, start_node, last_node);
			return false;
		}
	}
}

static void lf_do_free(struct buddy_lf_ctx *ctx, unsigned long start_page_idx,
		       unsigned free_order)
{
	unsigned height = lf_height(ctx);
	unsigned level = height - free_order;
	unsigned long level_offset =
			(1UL << ((unsigned)ctx->order + 1 - level))
			* BUDDY_PAGE_SIZE;
	/*
	 * Rust free() uses byte offset (alloc returns virt byte addr); we use
	 * page index from struct page. Convert: byte_off = page_idx * PAGE_SIZE.
	 */
	unsigned long byte_off =
			(start_page_idx - ctx->start_page) * BUDDY_PAGE_SIZE;
	u32 node_pos = (u32)((1UL << (level - 1)) + byte_off / level_offset);

	lf_free_node(ctx, lf_node_at(ctx, node_pos), lf_node_at(ctx, 1));
}

void init_buddy_lf(int cxl_pool_idx, struct phys_mem_pool *pool,
		   page_type_t type, paddr_t free_mem_start,
		   paddr_t free_mem_end)
{
	paddr_t base = free_mem_start;
	paddr_t end = free_mem_end;
	u8 ord;
	unsigned long lf_npages = 0;
	size_t meta_sz = 0;
	size_t tree_bytes = 0;
	paddr_t pm_unaligned = 0;
	paddr_t free_page_start = 0;
	struct page *page_meta_start = NULL;
	void *lf_base = NULL;
	struct lf_node *tree = NULL;
	struct lf_node_container *containers = NULL;
	unsigned long nodes_count;
	int i;

	BUG_ON(type != CXL_MEM_PAGE);
	BUG_ON(cxl_pool_idx < 0 || cxl_pool_idx >= N_PHYS_MEM_POOLS);
	BUG_ON(cxl_pool_idx >= cxlmem_map_num);

	for (ord = 20; ord >= 1; ord--) {
		lf_npages = 1UL << ord;
		meta_sz = lf_reserved_meta_bytes(ord);
		pm_unaligned = base + meta_sz;
		if (pm_unaligned < base)
			continue;
		pm_unaligned = ROUND_UP(pm_unaligned, 16);
		free_page_start =
				ROUND_UP(pm_unaligned + lf_npages * sizeof(struct page),
					 PAGE_SIZE);
		if (free_page_start + lf_npages * PAGE_SIZE <= end
		    && free_page_start >= base)
			break;
	}
	BUG_ON(ord < 1);

	lf_base = (void *)phys_to_virt(base);
	memset(lf_base, 0, meta_sz);
	tree = (struct lf_node *)lf_base;
	nodes_count = lf_num_nodes_from_order(ord);
	tree_bytes = (size_t)(nodes_count + 1) * sizeof(struct lf_node);
	containers = (struct lf_node_container *)((unsigned char *)lf_base
						  + ROUND_UP(tree_bytes, 8));

	page_meta_start = (struct page *)(void *)phys_to_virt(pm_unaligned);
	memset(page_meta_start, 0, lf_npages * sizeof(struct page));

	if (lock_init(&pool->buddy_lock) != 0)
		BUG("buddy_lf lock_init failed\n");

	pool->pool_start_addr = (vaddr_t)phys_to_virt(free_page_start);
	pool->page_metadata = page_meta_start;
	pool->pool_mem_size = lf_npages * BUDDY_PAGE_SIZE;
	pool->pool_phys_page_num = lf_npages;
	pool->type = type;

	for (i = 0; i < BUDDY_MAX_ORDER; ++i) {
		pool->free_lists[i].nr_free = 0;
		init_list_head(&pool->free_lists[i].free_list);
	}

	for (i = 0; (unsigned long)i < lf_npages; ++i) {
		struct page *p = page_meta_start + i;

		p->pool = pool;
		p->order = 0;
		p->flags = 0;
#ifdef RMAP_ENABLED
		p->pmo = NULL;
		p->index = 0;
#endif
#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
		p->page_pair = 0;
#endif
	}

	buddy_lf_cxl[cxl_pool_idx].pool = pool;
	buddy_lf_cxl[cxl_pool_idx].tree = tree;
	buddy_lf_cxl[cxl_pool_idx].containers = containers;
	buddy_lf_cxl[cxl_pool_idx].order = ord;
	buddy_lf_cxl[cxl_pool_idx].start_page = 0;

	memset(tree, 0, (nodes_count + 1) * sizeof(struct lf_node));
	memset(containers, 0, (nodes_count - 1) * sizeof(struct lf_node_container));

	lf_init_tree(&buddy_lf_cxl[cxl_pool_idx]);

	kinfo("[BUDDY_LF_INIT] pool %p type %d ord %u lf_npages %lu meta 0x%lx "
	      "pages @ 0x%lx\n",
	      pool, type, ord, lf_npages, (unsigned long)meta_sz,
	      (unsigned long)free_page_start);
}

struct page *buddy_lf_get_pages(struct phys_mem_pool *pool, int order)
{
	struct buddy_lf_ctx *ctx = lf_ctx_for_pool(pool);
	unsigned long page_idx;
	struct page *page;
	int i;

	if (unlikely(order >= BUDDY_MAX_ORDER)) {
		kwarn("buddy_lf: order too large\n");
		return NULL;
	}
	if (!ctx)
		return NULL;
	if (unlikely((unsigned)order > (unsigned)ctx->order)) {
		kwarn("buddy_lf: order %d > tree order %u\n", order, ctx->order);
		return NULL;
	}

	if (!lf_do_alloc(ctx, (unsigned)order, &page_idx))
		return NULL;

	page = pool->page_metadata + page_idx;
	for (i = 0; i < (1 << order); i++) {
		struct page *p = page + i;

#ifdef RMAP_ENABLED
		set_compound_head(p, page);
#endif
		page_set_flag(p, PG_allocated);
	}
	page->order = order;
	return page;
}

void buddy_lf_free_pages(struct phys_mem_pool *pool, struct page *page)
{
	struct buddy_lf_ctx *ctx = lf_ctx_for_pool(pool);
	unsigned long idx;
	int ord = page->order;
	int i;

	if (!ctx)
		BUG("buddy_lf_free_pages: no ctx\n");

#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
	prepare_latest_log(pool, REMOVE_PAGES, (u64)page, page->order, 0);
#endif
	for (i = 0; i < (1 << ord); i++) {
		struct page *p = page + i;

		BUG_ON(!page_check_flag(p, PG_allocated));
		p->flags = 0;
#ifdef RMAP_ENABLED
		clear_compound_head(p);
		p->pmo = NULL;
		p->index = 0;
#endif
#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
		p->page_pair = 0;
#endif
	}
#if defined(CHCORE_SLS) && defined(HYBRID_MEM)
	if (page->track_info)
		destory_track_info(page);
#endif
	idx = (unsigned long)(page - pool->page_metadata);
	lf_do_free(ctx, idx, (unsigned)ord);

#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
	commit_latest_log(pool);
#endif
}

#endif /* USE_CXL_MEM */

