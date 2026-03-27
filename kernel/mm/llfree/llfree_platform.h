#pragma once

#include <common/kprint.h>
#include <common/macro.h>
#include <common/util.h> /* memcpy */
#include <arch/sync.h>
#include <mm/llfree/llfree_types.h>

/*
 * Kernel platform layer for llfree.
 * Replaces linux userland stdatomic/stdio/assert usage.
 */

#define ll_align(align) __attribute__((aligned(align)))
#define unlikely(x) __builtin_expect(!!(x), 0)

#define PRIuS "lu"
#define PRIxS "lx"

#define llfree_warn(str, ...) kwarn("llfree: " str "\n", ##__VA_ARGS__)
#define llfree_info(str, ...) kinfo("llfree: " str "\n", ##__VA_ARGS__)
#define llfree_debug(str, ...) (void)0

/*
 * llfree is used on the shared CXL pool. Under heavy contention or transiently
 * inconsistent shared states, unbounded CAS loops can hard-hang the kernel.
 * Bound retries to preserve forward progress and surface the issue via logs.
 */
#ifndef LLFREE_ATOM_RETRY_LIMIT
#define LLFREE_ATOM_RETRY_LIMIT 1000000UL
#endif

#define llfree_assert(ok)                                                        \
	do {                                                                     \
		if (unlikely(!(ok)))                                             \
			BUG("llfree assert failed at %s:%d (%s)\n", __FILE__,     \
			    __LINE__, __func__);                                \
	} while (0)

/*
 * llfree uses atom_* macros in the linux version. We implement them on top of
 * ChCore's arch atomics.
 *
 * Important: llfree uses 32-bit atomics for tree_t and 64-bit atomics for
 * bitfield rows. We select 32/64 paths based on sizeof(*obj).
 */

static inline bool ll_atom_cmp_exchange_32(s32 *obj, s32 *expected, s32 desired)
{
	/*
	 * On CAS failure, expected must be updated with the value observed by the
	 * failed CAS operation (not a later re-load), otherwise lock-free retry
	 * loops can skip states and violate invariants.
	 */
	s32 exp = *expected;
	s32 old = compare_and_swap_32(obj, exp, desired);
	if (old == exp)
		return true;
	*expected = old;
	return false;
}

static inline bool ll_atom_cmp_exchange_64(s64 *obj, s64 *expected, s64 desired)
{
	s64 exp = *expected;
	s64 old = compare_and_swap_64(obj, exp, desired);
	if (old == exp)
		return true;
	*expected = old;
	return false;
}

static inline s32 ll_atom_swap_32(s32 *obj, s32 desired)
{
	unsigned long retries = 0;
	for (;;) {
		s32 old = atomic_load_32(obj);
		if (atomic_bool_compare_exchange_32(obj, old, desired))
			return old;
		retries++;
		if (unlikely(retries > LLFREE_ATOM_RETRY_LIMIT)) {
			llfree_warn("atom_swap_32 livelock obj=%p", obj);
			return atomic_load_32(obj);
		}
		CPU_PAUSE();
	}
}

static inline s64 ll_atom_swap_64(s64 *obj, s64 desired)
{
	return atomic_exchange_64((void *)obj, desired);
}

static inline u32 ll_mem_to_u32(const void *p)
{
	u32 v;
	memcpy(&v, p, sizeof(v));
	return v;
}

static inline u64 ll_mem_to_u64(const void *p)
{
	u64 v;
	memcpy(&v, p, sizeof(v));
	return v;
}

static inline void ll_u32_to_mem(void *p, u32 v)
{
	memcpy(p, &v, sizeof(v));
}

static inline void ll_u64_to_mem(void *p, u64 v)
{
	memcpy(p, &v, sizeof(v));
}

#define atom_cmp_exchange(obj, expected, desired)                                  \
	({                                                                         \
		bool _ok;                                                          \
		typeof(*(obj)) _des_tmp = (desired);                               \
		if (sizeof(*(obj)) == 4) {                                         \
			u32 _exp_u = ll_mem_to_u32((expected));                     \
			u32 _des_u = ll_mem_to_u32(&_des_tmp);                      \
			s32 _exp_s = (s32)_exp_u;                                   \
			_ok = ll_atom_cmp_exchange_32((s32 *)(obj), &_exp_s,         \
						      (s32)_des_u);               \
			ll_u32_to_mem((expected), (u32)_exp_s);                     \
		} else {                                                           \
			u64 _exp_u = ll_mem_to_u64((expected));                     \
			u64 _des_u = ll_mem_to_u64(&_des_tmp);                      \
			s64 _exp_s = (s64)_exp_u;                                   \
			_ok = ll_atom_cmp_exchange_64((s64 *)(obj), &_exp_s,         \
						      (s64)_des_u);               \
			ll_u64_to_mem((expected), (u64)_exp_s);                     \
		}                                                                  \
		_ok;                                                               \
	})

#define atom_cmp_exchange_weak(obj, expected, desired) atom_cmp_exchange(obj, expected, desired)

#define atom_swap(obj, desired)                                                    \
	({                                                                          \
		typeof(*(obj)) _ret;                                                \
		typeof(*(obj)) _des_tmp = (desired);                                \
		if (sizeof(*(obj)) == 4) {                                          \
			u32 _des_u = ll_mem_to_u32(&_des_tmp);                       \
			u32 _old_u = (u32)ll_atom_swap_32((s32 *)(obj), (s32)_des_u); \
			ll_u32_to_mem(&_ret, _old_u);                                \
		} else {                                                            \
			u64 _des_u = ll_mem_to_u64(&_des_tmp);                       \
			u64 _old_u = (u64)ll_atom_swap_64((s64 *)(obj), (s64)_des_u); \
			ll_u64_to_mem(&_ret, _old_u);                                \
		}                                                                   \
		_ret;                                                               \
	})

#define atom_load(obj)                                                            \
	({                                                                          \
		typeof(*(obj)) _ret;                                                \
		if (sizeof(*(obj)) == 4) {                                          \
			u32 _v = (u32)atomic_load_32((s32 *)(obj));                 \
			ll_u32_to_mem(&_ret, _v);                                   \
		} else {                                                            \
			u64 _v = (u64)atomic_load_64((s64 *)(obj));                 \
			ll_u64_to_mem(&_ret, _v);                                   \
		}                                                                   \
		_ret;                                                               \
	})

#define atom_store(obj, val)                                                      \
	({                                                                          \
		typeof(*(obj)) _val_tmp = (val);                                    \
		if (sizeof(*(obj)) == 4) {                                          \
			u32 _v = ll_mem_to_u32(&_val_tmp);                          \
			atomic_store_32((s32 *)(obj), (s32)_v);                     \
		} else {                                                            \
			u64 _v = ll_mem_to_u64(&_val_tmp);                          \
			atomic_store_64((s64 *)(obj), (s64)_v);                     \
		}                                                                   \
	})

#define atom_update(atom_ptr, old_val, fn, ...)                                   \
	({                                                                        \
		bool _ret = false;                                                \
		unsigned long _retries = 0;                                       \
		(old_val) = atom_load(atom_ptr);                                   \
		while (true) {                                                     \
			__typeof(old_val) value = (old_val);                       \
			if (!(fn)(&value, ##__VA_ARGS__))                          \
				break;                                            \
			__typeof(old_val) _exp = (old_val);                         \
			if (atom_cmp_exchange(atom_ptr, &_exp, value)) {            \
				_ret = true;                                       \
				break;                                            \
			}                                                           \
			(old_val) = _exp;                                           \
			_retries++;                                                  \
			if (unlikely(_retries > LLFREE_ATOM_RETRY_LIMIT)) {          \
				llfree_warn("atom_update livelock atom=%p", (atom_ptr)); \
				break;                                              \
			}                                                           \
			CPU_PAUSE();                                                 \
		}                                                                   \
		_ret;                                                              \
	})
