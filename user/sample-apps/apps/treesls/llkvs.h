#include <stdlib.h>
#include <string.h>
#define P 1226959
#define kvs_assert(x)                                                 \
        do {                                                          \
                if (!(x)) {                                           \
                        printf("kvs_assertion failed: %s, line %d\n", \
                               __FILE__,                              \
                               __LINE__);                             \
                }                                                     \
        } while (0)

typedef struct {
    void *data;
    long long len;
} span_t;

typedef long long kvs_key_t;
typedef span_t kvs_value_t;

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
};

inline static void *kvs_alloc(unsigned int size) {
    return malloc(size);
}

inline static void kvs_free(void *ptr) {
    free(ptr);
}

inline static int kvs_key_equals(const kvs_key_t *key1, const kvs_key_t *key2) {
    return *key1 == *key2;
}

inline static unsigned int kvs_hash(const kvs_key_t *key) {
    return (unsigned int)(unsigned long long)*key * P;
}

inline static struct kvs *new_kvs(unsigned int size) {
    /* Alloc memory for the hash table */
    struct kvs *kv = (struct kvs *)kvs_alloc(sizeof(struct kvs) + size * sizeof(void*));
    kvs_assert(kv);
    
    /* Initialize */
    kv->size = size;
    memset(kv->buckets, 0, size * sizeof(void*));
    
    return kv;
}

/* NOTE: this does not check if the key already exists */
inline static int kvs_put(struct kvs *kv, const kvs_key_t *key, const kvs_value_t *value) {
    /* Alloc new node */
    struct kvs_node *node = (struct kvs_node *)kvs_alloc(sizeof(*node));
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

    return 0;
}

inline static kvs_value_t *kvs_get(const struct kvs *kv, const kvs_key_t *key) {
    /* Calculate bucket index */
    int index = kvs_hash(key) % kv->size;

    /* Traverse link-list */
    struct kvs_node *node = kv->buckets[index];
    while (node && !kvs_key_equals(&node->key, key)) {
        node = node->next;
    }
    
    return node ? &node->value : NULL;
}

inline static int kvs_del(struct kvs *kv, const kvs_key_t *key) {
    /* Calculate bucket index */
    int index = kvs_hash(key) % kv->size;

    /* Find in link-list */
    struct kvs_node **last_ptr = kv->buckets + index, *current = kv->buckets[index];
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

inline static void kvs_clear(struct kvs *kv) {
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

inline static void kvs_destroy(struct kvs *kv) {
    kvs_clear(kv);
    kvs_free(kv);
}
