#include <mm/rmap.h>
#include <mm/buddy.h>
#include <arch/mmu.h>
#include <arch/mm/page_table.h>
#include <ckpt/ckpt_data.h>
#include <mm/nvm.h>

/*
 * pmo_add_reverse_node: add reverse node to page->reverse_list.
 */
void pmo_add_reverse_node(struct pmobject *pmo, struct vmregion *vmr)
{
	struct reverse_node *rnode, *tmp;
	bool has_vmr = false;

    lock(&(pmo->reverse_list_lock));
    
    for_each_in_list(tmp, struct reverse_node, node, &(pmo->reverse_list)) {
		if (tmp->vmr == vmr) {
            /* Already tracked this vmregion */
            has_vmr = true;
            break;
        }
    }

    /* Track vmregion info */
    if (!has_vmr) {
        rnode = kmalloc(sizeof(*rnode));
	    rnode->vmr = vmr;
        list_add(&(rnode->node), &(pmo->reverse_list));
    }

    unlock(&(pmo->reverse_list_lock));
}

/*
 * pmo_remove_reverse_node: remove reverse node.
 */
void pmo_remove_reverse_node(struct pmobject *pmo, struct vmregion *vmr)
{
	struct reverse_node *rnode, *tmp;

    lock(&pmo->reverse_list_lock);
    for_each_in_list_safe(rnode, tmp, node, &pmo->reverse_list) {
		if (rnode->vmr == vmr) {
            list_del(&(rnode->node));
            kfree(rnode);
        }
    }
    unlock(&pmo->reverse_list_lock);
}
