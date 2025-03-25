#pragma once

#include <mm/kmalloc.h>
#include <common/macro.h>
#include <common/util.h>
#include <irq/timer.h>
#include <perf/measure.h>
#define P 1226959

typedef void *kvs_key_t;
typedef void *kvs_value_t;

struct kvs_node {
    kvs_key_t key;
    kvs_value_t value;
    struct kvs_node *next;
};

struct kvs_key_node {
    kvs_key_t key;
    struct kvs_key_node *next;
};

struct kvs {
    unsigned int size;
    struct kvs_node *buckets[0];
    int type;
};

typedef struct kvs_node *kvs_list_t;

inline static void *kvs_alloc(unsigned int size, int type)
{
    return kmalloc(size, type);
}

inline static void kvs_free(void *ptr)
{
    kfree(ptr);
}

inline static int kvs_key_equals(const kvs_key_t *key1, const kvs_key_t *key2)
{
    return *key1 == *key2;
}

inline static unsigned int kvs_hash(const kvs_key_t *key)
{
    return (unsigned int)(((u64)*key) >> 5) * P;
}

inline static struct kvs *new_kvs(unsigned int size, int type)
{
    /* Alloc memory for the hash table */
    struct kvs *kv =
            (struct kvs *)kvs_alloc(sizeof(struct kvs) + size * sizeof(void *), type);
    BUG_ON(!kv);

    /* Initialize */
    kv->size = size;
    memset(kv->buckets, 0, size * sizeof(void *));
    kv->type = type;

    return kv;
}

#ifdef REPORT
u64 kvs_put_time;
#endif
/* NOTE: this does not check if the key already exists */
inline static int kvs_put(struct kvs *kv, const kvs_key_t *key,
                          const kvs_value_t *value)
{
    /* Alloc new node */
#ifdef REPORT
    DECLTMR;
    start();
#endif
    struct kvs_node *node;
    
    BUG_ON(kv->type < __DEFAULT__ || kv->type >= __MAX_MALLOC_TYPE__);
    
    node = (struct kvs_node *)kvs_alloc(sizeof(*node), kv->type);
    if (!node) {
        return 1;
    }

    /* Calculate bucket index */
    int index = kvs_hash(key) % kv->size;

    /* Fill fields */
    node->key = *key;
    node->value = *value;
    node->next = kv->buckets[index];

    /* Update table */
    kv->buckets[index] = node;
#ifdef REPORT
    kvs_put_time += stop();
#endif
    return 0;
}

#ifdef REPORT
extern u64 kvs_get_time;
#endif

inline static kvs_value_t *kvs_get(const struct kvs *kv, const kvs_key_t *key)
{
    /* Calculate bucket index */
#ifdef REPORT
    DECLTMR;
    start();
#endif
    int index = kvs_hash(key) % kv->size;

    /* Traverse link-list */
    struct kvs_node *node = kv->buckets[index];
    while (node && !kvs_key_equals(&node->key, key))
        node = node->next;
#ifdef REPORT
    kvs_get_time += stop();
#endif
    return node ? &node->value : NULL;
}

inline static int kvs_del(struct kvs *kv, const kvs_key_t *key)
{
    /* Calculate bucket index */
    int index = kvs_hash(key) % kv->size;

    /* Find in link-list */
    struct kvs_node **last_ptr = kv->buckets + index,
                    *current = kv->buckets[index];
    while (current && !kvs_key_equals(&current->key, key)) {
        last_ptr = &current->next;
        current = current->next;
    }

    /* Delete the node */
    if (current) {
        *last_ptr = current->next;
        kvs_free(current);
    }

    return 0;
}

inline static void kvs_clear(struct kvs *kv)
{
    int i;
    for (i = 0; i < kv->size; i++) {
        /* Clear one bucket */
        struct kvs_node *ptr = kv->buckets[i], *next;
        kv->buckets[i] = NULL;
        while (ptr) {
            next = ptr->next;
            kvs_free(ptr);
            ptr = next;
        }
    }
}

inline static void kvs_destroy(struct kvs *kv)
{
    kvs_clear(kv);
    kvs_free(kv);
}
