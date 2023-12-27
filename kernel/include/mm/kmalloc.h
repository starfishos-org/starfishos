#pragma once

int size_to_page_order(unsigned long size);

#if 0
void *kmalloc(unsigned long long size, __DEFAULT__);
void *kzalloc(unsigned long long size, __DEFAULT__);
void kfree(void *ptr);

/* Return vaddr of (1 << order) continous free physical pages */
void *get_pages(int order, __DEFAULT__);
void free_pages(void *addr);
#else

enum malloc_type {
        __DEFAULT__ = 0,
        __SHARED__,
};

void *kmalloc(unsigned long long size, int flags);
void *kzalloc(unsigned long long size, int flags);
void kfree(void *ptr);

/* Return vaddr of (1 << order) continous free physical pages */
void *get_pages(int order, int flags);
void free_pages(void *addr);
#endif

// TODO: merge several different kmalloc into kmalloc(size_t size, int flags)
/* DRAM */
void *dram_kmalloc(unsigned long long size);
void *dram_kzalloc(unsigned long long size);
// void kfree(void *ptr);

void *get_dram_pages(int order);
// void free_dram_pages(void *addr);

/* CXL */
void *cxl_kmalloc(unsigned long long size);
void *cxl_kzalloc(unsigned long long size);
// void cxl_kfree(void *ptr);

void *get_cxl_pages(int order);
// void free_cxl_pages(void *addr);
