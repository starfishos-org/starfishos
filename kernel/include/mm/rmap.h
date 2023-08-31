#include <common/types.h>
#include <object/thread.h>
#include <mm/buddy.h>
#include <mm/vmspace.h>
#include <arch/mm/page_table.h>

void pmo_add_reverse_node(struct pmobject *pmo, struct vmregion *vmr);
void pmo_remove_reverse_node(struct pmobject *pmo, struct vmregion *vmr);

#if 0
u64 page_get_index_by_reverse_node(struct page *page, struct reverse_node *rnode);
#endif
