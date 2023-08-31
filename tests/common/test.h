#pragma once
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <common/list.h>

//#define RND_NODES (10000)
#define RND_NODES (10000 * 100)
//#define RND_NODES (3)
#define RND_SEED (1024)
#define VALUE_MAX (123456LLU)
static inline u64 rand_value(u64 max)
{
	return (rand() % max) * (rand() % max) % max;
}

struct dummy_data {
	struct list_head node;
	struct hlist_node hnode;
	u64 key;
	u64 value;
	int index;
	bool deleted;
};

int compare_kv(const void *_a, const void *_b)
{
	const struct dummy_data *a = _a;
	const struct dummy_data *b = _b;

	if (a->key == b->key)
		return a->value - b->value;
	return a->key - b->key;
}

int compare_index(const void *_a, const void *_b)
{
	const struct dummy_data *a = _a;
	const struct dummy_data *b = _b;

	return a->index - b->index;
}

int compare_u32(const void *a, const void *b)
{
	return *(u32 *)a - *(u32 *)b;
}

int compare_u64(const void *a, const void *b)
{
	u64 ua = *(u64*)a;
	u64 ub = *(u64*)b;

	if (ua > ub) return 1;
	if (ua == ub) return 0;
	if (ua < ub) return -1;
}
