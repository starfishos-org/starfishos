#pragma once

void *kmalloc(unsigned long long size);
void *kzalloc(unsigned long long size);
void kfree(void *ptr);

/* Return vaddr of (1 << order) continous free physical pages */
void *get_pages(int order);
void free_pages(void *addr);

// TODO: merge several different kmalloc into kmalloc(size_t size, int flags)
/* DRAM */
void *dram_kmalloc(unsigned long long size);
void *dram_kzalloc(unsigned long long size);
void dram_kfree(void *ptr);

void *get_dram_pages(int order);
void free_dram_pages(void *addr);

/* CXL */
void *cxl_kmalloc(unsigned long long size);
void *cxl_kzalloc(unsigned long long size);
void cxl_kfree(void *ptr);

void *get_cxl_pages(int order);
void free_cxl_pages(void *addr);
