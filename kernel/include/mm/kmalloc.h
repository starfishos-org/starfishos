#pragma once

#ifdef DSM_ENABLED
#include <dsm/dsm-mmconfig.h>
#endif

int size_to_page_order(unsigned long size);

void *kmalloc(unsigned long long size, mem_t flags);
void *kzalloc(unsigned long long size, mem_t flags);
void kfree(void *ptr);

/* Return vaddr of (1 << order) continous free physical pages */
void *get_pages(int order, mem_t flags);
void free_pages(void *addr);

/* DRAM */
void *dram_kmalloc(unsigned long long size);
void *get_dram_pages(int order);

/* CXL */
void *cxl_kmalloc(unsigned long long size);
void *get_cxl_pages(int order);

#ifdef DSM_LINEAR_MM_LAYOUT
/* Temporal */
void *temp_kmalloc(unsigned long long size);
void *get_temp_pages(int order);
#else
#define temp_kmalloc(size)    kmalloc(size, __MT_DEFAULT__)
#define get_temp_pages(order) get_pages(order, __MT_DEFAULT__)
#endif
