#pragma once

#include <chcore/type.h>

struct hashmap;

struct hashmap *create_hashmap(u32 size);
void del_hashmap(struct hashmap *hm);
void hashmap_add(struct hashmap *hm, u64 key, void *val);
void *hashmap_del(struct hashmap *hm, u64 key);
void *hashmap_get(struct hashmap *hm, u64 key);
bool hashmap_empty(struct hashmap *hm);
