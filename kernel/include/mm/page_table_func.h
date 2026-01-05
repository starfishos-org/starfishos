#pragma once

#include <arch/mm/page_table.h>
#include <mm/vmspace.h>

int query_in_pgtbl(void *pgtbl, vaddr_t va, paddr_t *pa, pte_t **entry);
int query_in_all_pgtbls(void **pgtbls, size_t pgtbl_cnt, vaddr_t va, paddr_t *pa, pte_t **entry);
pte_t get_and_clear_pte(pte_t *pte);
int remap_page_in_pgtbl(pte_t *entry, paddr_t new_pa);
int set_pte_flags(pte_t *entry, vmr_prop_t flags, int kind);
int set_pte_write_flag(pte_t *entry, bool flag);
int is_pte_dirty(pte_t *entry);
void clear_pte_dirty(pte_t *entry);
void set_migration_entry(pte_t *pte);
int is_migration_entry(pte_t *pte);
#ifdef MULTI_PAGETABLE_ENABLED
int check_pgtbl_consistency(struct vmspace *vmspace);
#endif
