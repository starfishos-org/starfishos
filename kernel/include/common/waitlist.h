#ifndef WAITLIST_H
#define WAITLIST_H

#include <common/list.h>
#include <mm/kmalloc.h>

/* waiting list */
struct wait_node {
    struct list_head list;
    void *data;
};
typedef struct wait_node wait_node_t;

static inline wait_node_t *add_to_waiting_list(struct list_head *list, void *data)
{
    struct wait_node *node = kmalloc(sizeof(struct wait_node), __PRIVATE__);
    node->data = data;
    list_append(&node->list, list);
    return node;
}

static inline void remove_from_waiting_list(struct list_head *list, wait_node_t *node)
{
    BUG_ON(!node);
    list_del(&node->list);
    kfree(node);
}

#define for_each_in_waitlist(elem, head)        \
    for_each_in_list(elem, wait_node_t, list, head)

#define for_each_in_waitlist_safe(elem, tmp, head) \
    for_each_in_list_safe (elem, tmp, list, head)

#endif /* WAITLIST_H */
