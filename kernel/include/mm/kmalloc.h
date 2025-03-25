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
    __PRIVATE__,
    __SHARED__,
    __MAX_MALLOC_TYPE__,
};

/* sepcial flag for each type of process state */
#ifdef DSM_STACK_MODE_CXL
#define __STACK_MALLOC_TYPE__ __SHARED__
#elif defined DSM_STACK_MODE_DRAM
#define __STACK_MALLOC_TYPE__ __PRIVATE__
#else
#define __STACK_MALLOC_TYPE_ __DEFAULT__
#endif

#ifdef DSM_PGTABLE_MODE_CXL
#define __PGTABLE_MALLOC_TYPE__ __SHARED__
#elif defined DSM_STACK_MODE_DRAM
#define __PGTABLE_MALLOC_TYPE__ __PRIVATE__
#else
#define __PGTABLE_MALLOC_TYPE__ __DEFAULT__
#endif

#ifdef DSM_THREADCTX_MODE_CXL
#define __THREADCTX_MALLOC_TYPE__ __SHARED__
#elif defined DSM_THREADCTX_MODE_DRAM
#define __THREADCTX_MALLOC_TYPE__ __PRIVATE__
#else
#define __THREADCTX_MALLOC_TYPE__ __DEFAULT__
#endif

#ifdef DSM_OBJECT_MODE_CXL
#define __OBJECT_MALLOC_TYPE__ __SHARED__
#elif defined DSM_OBJECT_MODE_DRAM
#define __OBJECT_MALLOC_TYPE__ __PRIVATE__
#else
#define __OBJECT_MALLOC_TYPE__ __DEFAULT__
#endif

#ifdef DSM_PAGE_MODE_CXL
#define __PAGE_MALLOC_TYPE__ __SHARED__
#elif defined DSM_PAGE_MODE_DRAM
#define __PAGE_MALLOC_TYPE__ __PRIVATE__
#else
#define __PAGE_MALLOC_TYPE__ __DEFAULT__
#endif

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
void *get_dram_pages(int order);

/* CXL */
void *cxl_kmalloc(unsigned long long size);
void *get_cxl_pages(int order);

#ifdef DSM_LINEAR_MM_LAYOUT
/* Temporal */
void *temp_kmalloc(unsigned long long size);
void *get_temp_pages(int order);
#else
#define temp_kmalloc(size)    kmalloc(size, __DEFAULT__)
#define get_temp_pages(order) get_pages(order, __DEFAULT__)
#endif
