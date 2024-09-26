#include "../ckpt_objects.h"
#include "../ckpt_object_pool.h"
#include "../ckpt_page.h"

extern int pgtbl_deep_copy(vaddr_t *src_pgtbl, vaddr_t *dst_pgtbl);

#ifdef PMO_CHECKSUM
unsigned long mem_checksum(unsigned char *start, int size)
{
    unsigned long sum = 0;
    int i;
    for (i = 0; i < size; i++)
        sum = sum * 307 + start[i] + 1;
    return sum;
}

u64 pmo_checksum(struct pmobject *pmo) {
    if (use_continuous_pages(pmo)) {
        if (pmo->dram_cache.array == NULL) {
            return (u64)mem_checksum((unsigned char *)phys_to_virt(pmo->start), pmo->size);
        } else {
            char *pages = kmalloc(pmo->size, __DEFAULT__);
            memcpy(pages, (unsigned char *)phys_to_virt(pmo->start), pmo->size);
            for (int i = 0; i < pmo->size / PAGE_SIZE; i++) {
                if (pmo->dram_cache.array[i]) {
                    memcpy(pages + i * PAGE_SIZE, (unsigned char *)phys_to_virt(pmo->dram_cache.array[i]), PAGE_SIZE);
                } 
            }
            u64 checksum = (u64)mem_checksum((unsigned char *)pages, pmo->size);
            kfree(pages);
            return checksum;
        }
    }
    else if (use_radix(pmo))
        return (u64)radix_checksum(pmo->radix);
    else 
        return 0;
}

static u64 __ckpt_page_radix_checksum(unsigned long initial, struct radix_node *node, int level)
{
    int i;
    struct ckpt_page_pair *page_pair;
    vaddr_t va;
    if (level == RADIX_LEVELS - 1) {
        for (i = 0; i < RADIX_NODE_SIZE; i++) {
            if (node->values[i]) {
                extern unsigned long hash_page(
                        unsigned long initial, unsigned char *page, int size);
                page_pair = node->values[i];
                va = page_pair->pages[0].version_number
                                     >= page_pair->pages[1].version_number ?
                             page_pair->pages[0].va :
                             page_pair->pages[1].va;
                initial = hash_page(initial, (unsigned char *)va, PAGE_SIZE);
            }
        }
        return initial;
    }
    for (i = 0; i < RADIX_NODE_SIZE; i++) {
        if (node->children[i]) {
            initial = __ckpt_page_radix_checksum(
                    initial, node->children[i], level + 1);
        }
    }
    return initial;
}


u64 ckpt_page_radix_checksum(struct radix *ckpt_radix)
{
    return __ckpt_page_radix_checksum(0, ckpt_radix->root, 0);
}

u64 ckpt_pmo_checksum(struct ckpt_pmobject *ckpt_pmo)
{
    return ckpt_page_radix_checksum(ckpt_pmo->radix);
}

bool compare_page(paddr_t pa1, paddr_t pa2){
    char *va1, *va2;
    va1 = (char *)phys_to_virt(pa1);
    va2 = (char *)phys_to_virt(pa2);
    u64 *s1, *s2;
    for (int i = 0; i < PAGE_SIZE; i += 64) {
        s1 = (u64 *)(va1 + i);
        s2 = (u64 *)(va2 + i);
        if (*s1 != *s2)
            return false;
    }
    return true;
}

static void __check_ckpt_page(struct radix_node *ckpt_page_node, struct radix_node *page_node, u64 prefix, int level)
{
    int i;
	prefix <<= RADIX_NODE_BITS;
	u64 idx;
    paddr_t pa;
    struct ckpt_page_pair *page_pair;
	if (level == 1) {
		/* the leaf level */
		for (i = 0; i < RADIX_NODE_SIZE; i++) {
            if (ckpt_page_node->values[i] || page_node->values[i]) {
				if (ckpt_page_node->values[i] && page_node->values[i]) {
                    idx = prefix | i;
                    page_pair = ckpt_page_node->values[i];
                    pa = (paddr_t)page_node->values[i];
                    int ckpt_page_idx = page_pair->pages[0].version_number >= page_pair->pages[1].version_number ? 0 : 1;
                    if (!compare_page(pa, virt_to_phys((void*)page_pair->pages[ckpt_page_idx].va))) {
                        printk("idx %d page %u is wrong, 0: %u, 1: %u\n",
                                idx, ckpt_page_idx, page_pair->pages[0].version_number, page_pair->pages[1].version_number);
                        if (compare_page(pa, virt_to_phys((void*)page_pair->pages[1 - ckpt_page_idx].va))) {
                            printk("we choose a wrong ckpt page\n");
                        }
                    }
                } else if (ckpt_page_node->values[i]) {
                    printk("ckpt_page_node has values but page_node not\n");
                } else {
                    printk("page_node has values but ckpt_page_node not\n"); 
                }
			}
		}
	}
	else {
		for (i = 0; i < RADIX_NODE_SIZE; i++) {
			if (ckpt_page_node->children[i] || page_node->children[i]) {
				if(ckpt_page_node->children[i] && page_node->children[i]) {
                    __check_ckpt_page(ckpt_page_node->children[i], page_node->children[i], prefix | i, level - 1);
                }else if(ckpt_page_node->children[i]){
                    printk("ckpt_page_node has children but page_node not\n");
                }else {
                    printk("page_node has children but ckpt_page_node not\n"); 
                }
			}
		}
	}
}

void check_ckpt_page(struct radix *ckpt_page_radix, struct radix *radix)
{
    return __check_ckpt_page(ckpt_page_radix->root, radix->root, 0, RADIX_LEVELS);
}

static int __check_ckpt_page2(struct radix_node *ckpt_page_node, struct radix_node *page_node, u64 prefix, int level)
{
    int i, r = 0;
	prefix <<= RADIX_NODE_BITS;
	u64 idx;
    paddr_t pa;
    struct ckpt_page_pair *page_pair;
	if (level == 1) {
		/* the leaf level */
		for (i = 0; i < RADIX_NODE_SIZE; i++) {
            if (ckpt_page_node->values[i] || page_node->values[i]) {
				if(ckpt_page_node->values[i] && page_node->values[i]) {
                    idx = prefix | i;
                    page_pair = ckpt_page_node->values[i];
                    pa = (paddr_t)page_node->values[i];
                    int ckpt_page_idx = page_pair->pages[0].version_number >= page_pair->pages[1].version_number ? 0 : 1;
                    if (!compare_page(pa, virt_to_phys((void*)page_pair->pages[ckpt_page_idx].va))) {
                        printk("idx %d page %u is wrong, 0: %u, 1: %u\n",
                               idx, ckpt_page_idx, page_pair->pages[0].version_number, page_pair->pages[1].version_number);
                        if (compare_page(pa, virt_to_phys((void*)page_pair->pages[1 - ckpt_page_idx].va))) {
                            printk("current version %d\n", get_current_ckpt_version());
                            printk("maybe we choose a wrong ckpt page\n");
                        }
                        r = -1;
                    }
                } else if(ckpt_page_node->values[i]){
                    printk("ckpt_page_node has values but page_node not\n");
                    r = -1;
                } else {
                    printk("page_node has values but ckpt_page_node not\n"); 
                    r = -1;
                }
			}
		}
	}
	else {
		for (i = 0; i < RADIX_NODE_SIZE; i++) {
			if (ckpt_page_node->children[i] || page_node->children[i]) {
				if (ckpt_page_node->children[i] && page_node->children[i]) {
                    int ret = __check_ckpt_page2(ckpt_page_node->children[i], page_node->children[i], prefix | i, level - 1);
                    if(ret < 0)
                        r = -1;
                } else if (ckpt_page_node->children[i]){
                    printk("ckpt_page_node has children but page_node not\n");
                } else {
                    printk("page_node has children but ckpt_page_node not\n"); 
                }
			}
		}
	}
    return r;
}

/* check ckpt_page when restore */
int check_ckpt_page2(struct radix *ckpt_page_radix, struct radix *radix)
{
    return __check_ckpt_page2(ckpt_page_radix->root, radix->root, 0, RADIX_LEVELS);
}

static int __compare_pmo_radix(struct radix_node *ckpt_page_node, struct radix_node *page_node, u64 prefix, int level)
{
    int i, r = 0;
	prefix <<= RADIX_NODE_BITS;
	u64 idx;
    paddr_t pa1, pa2;
	if (level == 1) {
		/* the leaf level */
		for (i = 0; i < RADIX_NODE_SIZE; i++) {
            if (ckpt_page_node->values[i] || page_node->values[i]) {
				if (ckpt_page_node->values[i] && page_node->values[i]) {
                    idx = prefix | i;
                    pa1 = (paddr_t)ckpt_page_node->values[i];
                    pa2 = (paddr_t)page_node->values[i];
                    if(!compare_page(pa1, pa2)) {
                        printk("idx %lu page is wrong\n", idx);
                        r = -1;
                    }
                } else if(ckpt_page_node->values[i]){
                    printk("ckpt_page_node has values but page_node not\n");
                    r = 0;
                } else {
                    printk("page_node has values but ckpt_page_node not\n"); 
                    r = -1;
                }
			}
		}
	}
	else {
		for (i = 0; i < RADIX_NODE_SIZE; i++) {
			if (ckpt_page_node->children[i] || page_node->children[i]) {
				if (ckpt_page_node->children[i] && page_node->children[i]) {
                    int ret = __compare_pmo_radix(ckpt_page_node->children[i], page_node->children[i], prefix | i, level - 1);
                    if(ret < 0)
                        r = -1;
                } else if (ckpt_page_node->children[i]) {
                    printk("ckpt_page_node has children but page_node not\n");
                } else {
                    printk("page_node has children but ckpt_page_node not\n");
                    r = -1; 
                }
			}
		}
	}
    return r;
}

int compare_pmo_radix(struct radix *radix1, struct radix *radix2)
{
    return __compare_pmo_radix(radix1->root, radix2->root, 0, RADIX_LEVELS);
}
#endif

inline void clear_ckpt_page(struct ckpt_page_pair *page_pair, int idx)
{
    /* only clear version number */
    page_pair->pages[idx].version_number = 0;
}

int choose_ckpt_page_idx(struct ckpt_page_pair *page_pair)
{
    u64 ckpt_version = get_current_ckpt_version();
    u64 ckpt_page_version[2] = {page_pair->pages[0].version_number,
                                page_pair->pages[1].version_number};
    if (ckpt_version >= ckpt_page_version[0]
        && ckpt_version >= ckpt_page_version[1]) {
        /* when the version number of page_0 and page_1 are equal,
        * we treat page_0 as the new one.
        */
        return ckpt_page_version[0] >= ckpt_page_version[1] ? 0 : 1;
    } else {
        BUG_ON(ckpt_version < ckpt_page_version[0]
                && ckpt_version < ckpt_page_version[1]);
        /* One of the page version numbers exceeding the checkpoint
        * version number should be discarded.
        * So we should choose the page with the small version number.
        */
        return ckpt_page_version[0] >= ckpt_page_version[1] ? 1 : 0;
    }
}

int choose_nvm_ckpt_page_idx(struct ckpt_page_pair *page_pair)
{
    u64 ckpt_version = get_current_ckpt_version();
    u64 ckpt_page_version[2] = {page_pair->pages[0].version_number,
                                page_pair->pages[1].version_number};
    if (ckpt_version == ckpt_page_version[0]
        || ckpt_version == ckpt_page_version[1]) {
        /* when the version number of page_0 and page_1 are equal,
        * we treat page_0 as the new one.
        */
        return ckpt_version == ckpt_page_version[0] ? 0 : 1;
    } else {
        BUG_ON(ckpt_version < ckpt_page_version[0]
                && ckpt_version < ckpt_page_version[1]);
        /* One of the page version numbers exceeding the checkpoint
        * version number should be discarded.
        * So we should choose the page with the small version number.
        */
        return ckpt_page_version[0] > ckpt_page_version[1] ? 1 : 0;
    }
}

static int __radix_pmo_restore(struct pmobject *pmo, struct radix_node *page_node, struct radix_node *ckpt_page_node, int node_level, u64 prefix)
{
	int err;
    int i;
    struct radix_node *new;
    struct ckpt_page_pair *page_pair;
    paddr_t pa;
    vaddr_t va;
    u64 current_version = get_current_ckpt_version();
   
    prefix <<= RADIX_NODE_BITS;

    if(node_level == RADIX_LEVELS - 1) {
        for(i = 0;i < RADIX_NODE_SIZE;i++) {
            if((page_pair = ckpt_page_node->values[i]) || page_node->values[i]) {
                pa = (paddr_t)page_node->values[i];
                BUG_ON(!pa);
                va = (vaddr_t)phys_to_virt(pa);
                /* set page info */
                struct page* page = virt_to_page((void*)va);
                page_type_t type = get_page_type(page);
                if (type == NVM_PAGE) {
                    if (page_pair) {
                        // int idx = choose_ckpt_page_idx(page_pair);
                        int idx = choose_nvm_ckpt_page_idx(page_pair);
                        int stale_idx = 1 - idx;

                        clear_ckpt_page(page_pair, stale_idx);
                        
                        BUG_ON(page_pair->pages[stale_idx].va == page_pair->pages[idx].va);
                        // BUG_ON(va != page_pair->pages[stale_idx].va);
                        
                        if(page_pair->pages[idx].version_number == current_version) {
                            pagecpy((void*)va, (void*)page_pair->pages[idx].va);
                        }
                    } else {
                    #ifdef PMO_CHECKSUM
                        struct page* page = virt_to_page((void*)phys_to_virt(pa));
                        if(page->ckpt_version_number > current_version) {
                            page_node->values[i] = NULL;
                            kfree((void*)phys_to_virt(pa));
                        }
                    #endif   
                    }
                } else {
                    BUG_ON(!page_pair);

                    int idx = choose_ckpt_page_idx(page_pair);
                    int stale_idx = 1 - idx;
                    
                    /* Use page_pair->pages[stale_idx] as runtime memory and page_pair->pages[idx] as backup */

                    BUG_ON(page_pair->pages[idx].version_number == 0);
                    
                    va = page_pair->pages[stale_idx].va;
                    clear_ckpt_page(page_pair, stale_idx);
                    pagecpy((void*)va, (void*)page_pair->pages[idx].va);
                    page_node->values[i] = (void*)virt_to_phys((void*)va);
                }
            }
        }
		return 0;
    }   

    for (i = 0; i < RADIX_NODE_SIZE;i++) {
        if (ckpt_page_node->children[i] || page_node->children[i]) {
            if (!page_node->children[i]) {
                new = kzalloc(sizeof(*new), __DEFAULT__);
                if(IS_ERR(new)) {
                    return -ENOMEM;
                }
                page_node->children[i] = new;
            }
            if (!ckpt_page_node->children[i]) {
                new = kzalloc(sizeof(*new), __DEFAULT__);
                if (IS_ERR(new)) {
                    return -ENOMEM;
                }
                ckpt_page_node->children[i] = new;
            }
            err = __radix_pmo_restore(pmo,
                                      page_node->children[i],
                                      ckpt_page_node->children[i],
                                      node_level + 1,
                                      prefix | i);
            if (err)
                return err;
        }
    }

    return 0;
}

int radix_pmo_restore(struct pmobject *pmo, struct ckpt_pmobject *ckpt_pmo)
{
	int r = 0;
    struct radix *pmo_radix = pmo->radix;
    struct radix *ckpt_page_radix = ckpt_pmo->radix;

	struct radix_node *new_node;
	lock(&pmo_radix->radix_lock);
	lock(&ckpt_page_radix->radix_lock);
	if (!ckpt_page_radix->root) {
        new_node = kzalloc(sizeof(*new_node), __DEFAULT__);
        if (IS_ERR(new_node)) {
            r = -ENOMEM;
        }
        ckpt_page_radix->root = new_node;
    }

    if (!pmo_radix->root) {
        new_node = kzalloc(sizeof(*new_node), __DEFAULT__);
        if (IS_ERR(new_node)) {
            r = -ENOMEM;
        }
        pmo_radix->root = new_node;
    }
	r = __radix_pmo_restore(pmo, pmo_radix->root, ckpt_page_radix->root, 0, 0);
	unlock(&ckpt_page_radix->radix_lock);
    unlock(&pmo_radix->radix_lock);
    return r;
}

static int __continuous_pmo_restore(struct pmobject *pmo, struct radix_node *ckpt_page_node, int node_level, u64 prefix)
{
	int err;
    int i;
    struct ckpt_page_pair *page_pair;
    paddr_t pa;
    vaddr_t va;
    u64 index;
    u64 current_version = get_current_ckpt_version();
   
    prefix <<= RADIX_NODE_BITS;

    if (node_level == RADIX_LEVELS - 1) {
        for (i = 0;i < RADIX_NODE_SIZE;i++) {
            if ((page_pair = ckpt_page_node->values[i])) {
                index = prefix | i;
                pa = (paddr_t)(pmo->start + index * PAGE_SIZE);
                va = (vaddr_t)phys_to_virt(pa);
                /* set page info */
                struct page* page = virt_to_page((void*)va);

                int idx = choose_ckpt_page_idx(page_pair);
                int stale_idx = 1 - idx;
                clear_ckpt_page(page_pair, stale_idx);
                if (pmo->dram_cache.array == NULL || pmo->dram_cache.array[index] == 0) {
                    /* page type == NVM */
                    BUG_ON(page_pair->pages[stale_idx].va == page_pair->pages[idx].va);
                    BUG_ON(va == page_pair->pages[idx].va || va != page_pair->pages[stale_idx].va);
                    if (page_pair->pages[idx].version_number == current_version) {
                        pagecpy((void*)va, (void*)page_pair->pages[idx].va);
                    }
                } else {
                    pagecpy((void*)page_pair->pages[stale_idx].va, (void*)page_pair->pages[idx].va);
                    if (va == page_pair->pages[idx].va) {    
                        page_pair->pages[stale_idx].version_number = current_version;
                        clear_ckpt_page(page_pair, idx);
                    }
                } 
            }
        }
		return 0;
    }   

    for (i = 0; i < RADIX_NODE_SIZE;i++) {
        if (ckpt_page_node->children[i]) {
            err = __continuous_pmo_restore(pmo,
                                      ckpt_page_node->children[i],
                                      node_level + 1,
                                      prefix | i);
            if (err)
                return err;
        }
    }

    return 0;
}

int continuous_pmo_restore(struct pmobject *pmo, struct radix *ckpt_page_radix)
{
	int r = 0;
	struct radix_node *new_node;
	lock(&ckpt_page_radix->radix_lock);
	if (!ckpt_page_radix->root) {
        new_node = kzalloc(sizeof(*new_node), __DEFAULT__);
        if(IS_ERR(new_node)) {
            r = -ENOMEM;
        }
        ckpt_page_radix->root = new_node;
    }

	r = __continuous_pmo_restore(pmo, ckpt_page_radix->root, 0, 0);
	unlock(&ckpt_page_radix->radix_lock);
    return r;
}

static int __init_ckpt_page_radix(struct radix_node *page_node, struct radix_node *ckpt_page_node, int node_level)
{
    int err;
    int i;
    struct radix_node *new;
    struct page *page;

    if (node_level == RADIX_LEVELS - 1) {
        for (i = 0;i < RADIX_NODE_SIZE;i++) {
            if (page_node->values[i]) {
                struct ckpt_page_pair *page_pair = kzalloc(sizeof(*page_pair), __DEFAULT__);
                if (!page_pair) {
                    return -ENOMEM;
                }
                // printk("%s: page=%p\n", __func__, virt_to_page((void*)phys_to_virt(page_node->values[i])));
                page_pair->pages[0].va = (vaddr_t)get_pages(0, __DEFAULT__);
                page_pair->pages[1].va = phys_to_virt(page_node->values[i]);
                pagecpy((void*)page_pair->pages[0].va, (void*)phys_to_virt(page_node->values[i]));
                page_pair->pages[0].version_number = get_current_ckpt_version();
                ckpt_page_node->values[i] = page_pair;
                page = (struct page*)virt_to_page((void*)phys_to_virt(page_node->values[i]));
                page->page_pair = (u64)page_pair;
            }
        }
		return 0;
    }

    for (i = 0; i < RADIX_NODE_SIZE; i++) {
        if (page_node->children[i]) {
            if (!ckpt_page_node->children[i]) {
                new = kzalloc(sizeof(*new), __DEFAULT__);
                if (IS_ERR(new)) {
                    return -ENOMEM;
                }
                ckpt_page_node->children[i] = new;
            }

            err = __init_ckpt_page_radix(page_node->children[i],
                                         ckpt_page_node->children[i],
                                         node_level + 1);
            if (err) {
                return err;
            }
        }
    }

    return 0;
}

int init_ckpt_page_radix(struct ckpt_pmobject* ckpt_pmo, struct pmobject *pmo)
{
    int r = 0;
    struct radix *pmo_radix = pmo->radix;
    struct radix *ckpt_page_radix;

    ckpt_pmo->radix = new_radix();
    init_radix(ckpt_pmo->radix);
    ckpt_page_radix = ckpt_pmo->radix;

    /* copy all pages in pmo to ckpt page in ckpt pmo*/
    if(use_radix(pmo)) {
        // lock(&pmo_radix->radix_lock);
        // lock(&ckpt_page_radix->radix_lock);
        r = __init_ckpt_page_radix(pmo_radix->root,
                                   ckpt_page_radix->root,
                                   0);
        // unlock(&ckpt_page_radix->radix_lock);
        // unlock(&pmo_radix->radix_lock);
    }

    return r;
}

int pmo_ckpt(struct pmobject *pmo, struct ckpt_pmobject *ckpt_pmo)
{ 
    /*Step 1: init ckpt pmo */
    int r;
    u64 current_ckpt_version;

    ckpt_pmo->private = pmo->private;
    ckpt_pmo->size = pmo->size;
    ckpt_pmo->type = pmo->type;

    current_ckpt_version = get_current_ckpt_version();

    if (use_continuous_pages(pmo) || use_radix(pmo)) {
        if (unlikely(!ckpt_pmo->radix)) {
            /* If the radix tree is not created, try to reuse the
             * previous version of the radix tree */
            struct ckpt_obj_root *pmo_root;
            struct ckpt_object *latest_ckpt_obj;
            pmo_root = ckpt_obj_root_get(container_of(pmo, struct object,opaque), false);
            if (pmo_root && (latest_ckpt_obj =
                    get_latest_ckpt_obj(pmo_root, current_ckpt_version))) {
                struct ckpt_pmobject *latest_ckpt_pmo =
                        (struct ckpt_pmobject*)latest_ckpt_obj->opaque;
                BUG_ON(!latest_ckpt_pmo->radix);
                ckpt_pmo->radix = latest_ckpt_pmo->radix;
            } else {
                /* The previous version of the radix tree is NULL, 
                * so we create a radix tree*/
                r = init_ckpt_page_radix(ckpt_pmo, pmo);
                if (r < 0) {
                    return r;
                }
            }
        }
    }
    /* check ckpt_pmo (for debug) */
#ifdef PMO_CHECKSUM
    if (use_radix(pmo)) {
        if (!ckpt_pmo->radix_backup) {
            ckpt_pmo->radix_backup = new_radix();
            init_radix(ckpt_pmo->radix_backup);
        }
        radix_deep_copy(pmo->radix, ckpt_pmo->radix_backup, true);
    } else {
        ckpt_pmo->checksum = pmo_checksum(pmo);
    }
    
    if (use_radix(pmo)) {
        u64 ckpt_checksum = ckpt_pmo_checksum(ckpt_pmo);
        if (ckpt_checksum != ckpt_pmo->checksum) {
            printk("type:%d, verison:%d, %lx pmo_ckpt erratic: %lx, %lx\n",
                   pmo->type,
                   get_current_ckpt_version(),
                   pmo,
                   ckpt_checksum,
                   ckpt_pmo->checksum);
            check_ckpt_page(ckpt_pmo->radix, pmo->radix);
        }
    }
#endif
    return 0;
}

int pmo_restore(struct object *pmo_obj, struct ckpt_object *ckpt_pmo_obj, struct kvs* obj_map)
{ 
    int r = 0;
    struct ckpt_pmobject *ckpt_pmo = (struct ckpt_pmobject *)ckpt_pmo_obj->opaque;
    struct pmobject *pmo = (struct pmobject *)pmo_obj->opaque;
#ifdef RESTORE_REPORT
    DECLTMR; start();
#endif	
    kdebug("[pmo_restore] pmo:%p, start:%lx, start_va:%lx, size:%lx, type:%d\n",
            pmo, pmo->start, phys_to_virt(pmo->start), pmo->size, pmo->type);
    
    lock_init(&ckpt_pmo->lock);

    /* restore pmo's field */
    pmo->private = ckpt_pmo->private;
    pmo->size = ckpt_pmo->size;
    pmo->type = ckpt_pmo->type;

#ifdef RMAP_ENABLED
    /* init reverse list and lock */ 
    lock_init(&pmo->reverse_list_lock);
    init_list_head(&pmo->reverse_list);
#endif
    
    if (use_continuous_pages(pmo)) {
        continuous_pmo_restore(pmo, ckpt_pmo->radix);
        pmo->dram_cache.array = NULL;
        lock_init(&pmo->dram_cache.lock);
#ifdef PMO_CHECKSUM
        if (pmo_checksum(pmo) != ckpt_pmo->checksum && !is_external_sync_pmo(pmo)) {
            printk("error\n");
            printk("[erratic continuous pmo %lx, type:%d] checksum:%lx, ckpt_checksum:%lx\n",
                   pmo, pmo->type, pmo_checksum(pmo), ckpt_pmo->checksum);
        } else {
            printk("[success continuous pmo %lx, type:%d] checksum:%lx, ckpt_checksum:%lx\n",
                   pmo, pmo->type, pmo_checksum(pmo), ckpt_pmo->checksum);
        }
#endif
    } else if (use_radix(pmo)) {
        /* restore radix tree */
        lock_init(&pmo->radix->radix_lock);
        r = radix_pmo_restore(pmo, ckpt_pmo);

#ifdef PMO_CHECKSUM
        if (!is_external_sync_pmo(pmo)) {
            printk("[pmo %lx, type:%d]\n", pmo, pmo->type);
            compare_pmo_radix(pmo->radix, ckpt_pmo->radix_backup);
        }
#endif
    }
#ifdef RESTORE_REPORT
    eval_restore_obj_time[TYPE_PMO] += stop();
#endif	
    return r; 
}


static int __radix_deep_copy_with_hybird_mem(struct radix_node *src, struct radix_node *dst, int node_level)
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
				void *src_pa = src->values[i];
				void *src_va = (void*)phys_to_virt(src_pa);
				struct page *page = virt_to_page(src_va);
				if(get_page_type(page) == NVM_PAGE) {
                    lock(&page->lock);
#ifdef RMAP_ENABLED
                    if(page->track_info) {
                        if (page->track_info->active) {
                            delete_from_active_list(page->track_info);
                        }
                    }
#endif
                    atomic_fetch_add_64(&page->ref_cnt,1);
                    unlock(&page->lock);
                    dst->values[i] = src_pa;
                }else {
                    void *newpage = get_pages(0, __DEFAULT__);
					BUG_ON(!newpage);
					pagecpy(newpage,
							(void *)phys_to_virt(src->values[i]));
					dst->values[i] =
							(void *)virt_to_phys(newpage);
                }
			}
		}
		return 0;
	}

	for (i = 0; i < RADIX_NODE_SIZE; i++) {
		if (src->children[i]) {
			new = kzalloc(sizeof(struct radix_node), __DEFAULT__);
			if (IS_ERR(new)) {
				return -ENOMEM;
			}
			dst->children[i] = new;
			err = __radix_deep_copy_with_hybird_mem(src->children[i],
									dst->children[i],
									node_level + 1);
			if (err) {
				return err;
			}
		}
	}

    return 0;
}


int radix_deep_copy_with_hybird_mem(struct radix *src,struct radix *dst)
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
        new = kzalloc(sizeof(struct radix_node), __DEFAULT__);
        if (IS_ERR(new)) {
            r = -ENOMEM;
        }
        dst->root = new;
    }

    r = __radix_deep_copy_with_hybird_mem(src->root, dst->root, 0);
out:
    unlock(&src->radix_lock);
    return r;
}