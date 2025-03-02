#ifdef CHCORE
#include <mm/kmalloc.h>
#include <common/kprint.h>
#include <common/macro.h>
#include <common/radix.h>
#include <mm/mm.h>
#include <mm/page.h>
#include <ckpt/ckpt_data.h>
#endif

#include <common/errno.h>

struct radix *new_radix(void)
{
    struct radix *radix;

    radix = kzalloc(sizeof(*radix), __DEFAULT__);
    BUG_ON(!radix);

    return radix;
}

void init_radix(struct radix *radix)
{
    radix->root = kzalloc(sizeof(*radix->root), __DEFAULT__);
    BUG_ON(!radix->root);
    radix->value_deleter = NULL;

    lock_init(&radix->radix_lock);
}

void init_radix_w_deleter(struct radix *radix, void (*value_deleter)(void *))
{
    init_radix(radix);
    radix->value_deleter = value_deleter;
}

static struct radix_node *new_radix_node(void)
{
    struct radix_node *n = kzalloc(sizeof(struct radix_node), __DEFAULT__);

    if (!n) {
        kwarn("run-out-memoroy: cannot allocate radix_new_node whose size is %ld\n",
              sizeof(struct radix_node));
        return ERR_PTR(-ENOMEM);
    }

    return n;
}

#ifndef FBINFER
int radix_add(struct radix *radix, u64 key, void *value)
{
    int ret;
    struct radix_node *node;
    struct radix_node *new;
    u16 index[RADIX_LEVELS];
    int i;
    int k;

    lock(&radix->radix_lock);
    if (!radix->root) {
        new = new_radix_node();
        if (IS_ERR(new)) {
            ret = -ENOMEM;
            goto fail_out;
        }
        radix->root = new;
    }
    node = radix->root;

    /* calculate index for each level */
    for (i = 0; i < RADIX_LEVELS; ++i) {
        index[i] = key & RADIX_NODE_MASK;
        key >>= RADIX_NODE_BITS;
    }

    /* the intermediate levels */
    for (i = RADIX_LEVELS - 1; i > 0; --i) {
        k = index[i];
        if (!node->children[k]) {
            new = new_radix_node();
            if (IS_ERR(new)) {
                ret = -ENOMEM;
                goto fail_out;
            }
            node->children[k] = new;
        }
        node = node->children[k];
    }

    /* the leaf level */
    k = index[0];

    if ((node->values[k] != NULL) && (value != NULL)) {
        kdebug("Radix: add an existing key\n");
        // BUG_ON(1);
        /* It is possible when using COW & migration */
    }

    node->values[k] = value;

    unlock(&radix->radix_lock);
    return 0;

fail_out:
    unlock(&radix->radix_lock);
    return ret;
}

void *radix_get(struct radix *radix, u64 key)
{
    void *ret;
    struct radix_node *node;
    u16 index[RADIX_LEVELS];
    int i;
    int k;

    lock(&radix->radix_lock);
    if (!radix->root) {
        ret = NULL;
        goto out;
    }
    node = radix->root;

    /* calculate index for each level */
    for (i = 0; i < RADIX_LEVELS; ++i) {
        index[i] = key & RADIX_NODE_MASK;
        key >>= RADIX_NODE_BITS;
    }

    /* the intermediate levels */
    for (i = RADIX_LEVELS - 1; i > 0; --i) {
        k = index[i];
        if (!node->children[k]) {
            ret = NULL;
            goto out;
        }
        node = node->children[k];
    }

    /* the leaf level */
    k = index[0];
    ret = node->values[k];

out:
    unlock(&radix->radix_lock);
    return ret;
}

/* FIXME(MK): We should allow users to store NULL in radix... */
int radix_del(struct radix *radix, u64 key)
{
    return radix_add(radix, key, NULL);
}

static void radix_free_node(struct radix_node *node, int node_level,
                            void (*value_deleter)(void *))
{
    int i;

    WARN_ON(!node, "should not try to free a node pointed by NULL");
    if (node_level == RADIX_LEVELS - 1) {
        if (value_deleter) {
            for (i = 0; i < RADIX_NODE_SIZE; i++) {
                if (node->values[i])
                    value_deleter(node->values[i]);
            }
        }
    } else {
        for (i = 0; i < RADIX_NODE_SIZE; i++) {
            if (node->children[i])
                radix_free_node(
                        node->children[i], node_level + 1, value_deleter);
        }
    }
    kfree(node);
}
#endif

int radix_free(struct radix *radix)
{
    lock(&radix->radix_lock);
    if (!radix || !radix->root) {
        WARN("trying to free an empty radix tree");
        return -EINVAL;
    }

    // recurssively free nodes and values (if value_deleter is not NULL)
    radix_free_node(radix->root, 0, radix->value_deleter);
    unlock(&radix->radix_lock);

    kfree(radix);
    return 0;
}

unsigned long hash_page(unsigned long initial, unsigned char *page, int size)
{
    int i;
    for (i = 0; i < size; i++) {
        initial = initial * 307 + page[i] + 1;
    }
    return initial;
}

static unsigned long __radix_checksum(unsigned long initial,
                                      struct radix_node *node, int level)
{
    int i;
    // initial = hash_page(initial, (unsigned char *)node, sizeof(*node));
    if (level == RADIX_LEVELS - 1) {
        for (i = 0; i < RADIX_NODE_SIZE; i++) {
            if (!node->values[i])
                continue;
            initial = hash_page(initial,
                                (unsigned char *)phys_to_virt(node->values[i]),
                                PAGE_SIZE);
        }
        return initial;
    }
    for (i = 0; i < RADIX_NODE_SIZE; i++) {
        if (node->children[i]) {
            initial = __radix_checksum(initial, node->children[i], level + 1);
        }
    }
    return initial;
}

extern void pagecpy(void *dst, const void *src);
static int __radix_deep_copy(struct radix_node *src, struct radix_node *dst,
                             int node_level, int phy_alloc)
{
    int err;
    int i;
    struct radix_node *new;
    if (node_level == RADIX_LEVELS - 1) {
        for (i = 0; i < RADIX_NODE_SIZE; i++) {
            if (!src->values[i]) {
                if (dst->values[i]) {
                    void *pa = dst->values[i];
                    dst->values[i] = NULL;
                    kfree((void *)phys_to_virt(pa));
                }
                continue;
            }

            if (dst->values[i]) {
                pagecpy((void *)phys_to_virt(dst->values[i]),
                        (void *)phys_to_virt(src->values[i]));
            } else {
                if (phy_alloc) {
                    void *newpage = get_pages(0, __DEFAULT__);
                    BUG_ON(!newpage);
                    pagecpy(newpage, (void *)phys_to_virt(src->values[i]));
                    dst->values[i] = (void *)virt_to_phys(newpage);
                } else {
                    dst->values[i] = src->values[i];
                }
            }
        }
        return 0;
    }

    for (i = 0; i < RADIX_NODE_SIZE; i++) {
        if (src->children[i]) {
            new = new_radix_node();
            if (IS_ERR(new)) {
                return -ENOMEM;
            }
            dst->children[i] = new;
            err = __radix_deep_copy(src->children[i],
                                    dst->children[i],
                                    node_level + 1,
                                    phy_alloc);
            if (err) {
                return err;
            }
        }
    }

    return 0;
}

unsigned long radix_checksum(struct radix *tree)
{
    return __radix_checksum(0, tree->root, 0);
}

/* if phy_alloc == true, we will alloc a new phy page to copy */
int radix_deep_copy(struct radix *src, struct radix *dst, int phy_alloc)
{
    int r;
    struct radix_node *new;
    BUG_ON(!(src && dst));
    r = 0;

    /* don't need to lock dst */
    lock(&src->radix_lock);

    if (!src->root) {
        goto out;
    }

    if (!dst->root) {
        new = new_radix_node();
        if (IS_ERR(new)) {
            r = -ENOMEM;
        }
        dst->root = new;
    }

    r = __radix_deep_copy(src->root, dst->root, 0, phy_alloc);
out:
    unlock(&src->radix_lock);
    return r;
}

static void __radix_traverse(struct radix_node *node, u64 prefix, int level,
                             radix_traverse_fn fn)
{
    int i;
    prefix <<= RADIX_NODE_BITS;
    vaddr_t va;
    if (level == 1) {
        /* the leaf level */
        for (i = 0; i < RADIX_NODE_SIZE; i++) {
            if (node->values[i]) {
                va = prefix | i;
                fn(va, (paddr_t)node->values[i]);
            }
        }
    } else {
        for (i = 0; i < RADIX_NODE_SIZE; i++) {
            if (node->children[i]) {
                __radix_traverse(node->children[i], prefix | i, level - 1, fn);
            }
        }
    }
}

void radix_traverse(struct radix *radix, radix_traverse_fn fn)
{
    lock(&radix->radix_lock);
    if (!radix->root) {
        goto finish;
    }
    struct radix_node *node = radix->root;
    __radix_traverse(node, 0, RADIX_LEVELS, fn);

finish:
    unlock(&radix->radix_lock);
}

static void *__radix_complex_traverse(struct radix_node *node, u64 prefix,
                                      int level, void *args,
                                      radix_complex_traverse_fn fn)
{
    int i;
    prefix <<= RADIX_NODE_BITS;
    vaddr_t va;
    void *res = NULL;
    if (level == 1) {
        /* the leaf level */
        for (i = 0; i < RADIX_NODE_SIZE; i++) {
            if (node->values[i]) {
                va = prefix | i;
                if ((res = fn(va, (paddr_t)node->values[i], args)) != NULL)
                    return res;
            }
        }
    } else {
        for (i = 0; i < RADIX_NODE_SIZE; i++) {
            if (node->children[i]) {
                return __radix_complex_traverse(
                        node->children[i], prefix | i, level - 1, args, fn);
            }
        }
    }
    return NULL;
}

void *radix_complex_traverse(struct radix *radix, void *args,
                             radix_complex_traverse_fn fn)
{
    void *res = NULL;

    lock(&radix->radix_lock);
    if (!radix->root) {
        goto finish;
    }

    res = __radix_complex_traverse(radix->root, 0, RADIX_LEVELS, args, fn);

finish:
    unlock(&radix->radix_lock);
    return res;
}
