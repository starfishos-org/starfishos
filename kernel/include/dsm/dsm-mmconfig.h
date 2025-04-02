#ifndef __DSM_MMCONFIG_H__
#define __DSM_MMCONFIG_H__

#include <uapi/types.h>

enum mem_type {
    __MT_INVALID__ = 0,
    __MT_PRIVATE__ = 1,
    __MT_SHARED__ = 2,
    __MT_MAX_TYPE__ = 3,
};

#define IS_VALID_MEM_TYPE(mt) ((mt) > __MT_INVALID__ && (mt) < __MT_MAX_TYPE__)

#if defined(DSM_MALLOC_MODE_MIXED_DEFAULT_DRAM) || defined(DSM_MALLOC_MODE_DRAM)
/* default to private */
#define __MT_DEFAULT__ __MT_PRIVATE__
#elif defined(DSM_MALLOC_MODE_MIXED_DEFAULT_CXL) || defined(DSM_MALLOC_MODE_CXL)
/* default to shared */
#define __MT_DEFAULT__ __MT_SHARED__
#else
#error "DSM_MALLOC_MODE must be defined"
#endif

/* sepcial flag for each type of process state */
#ifdef DSM_STACK_MODE_CXL
#define __MT_STACK__ __MT_SHARED__
#elif defined DSM_STACK_MODE_DRAM
#define __MT_STACK__ __MT_PRIVATE__
#elif defined DSM_STACK_MODE_MIXED
#define __MT_STACK__ __MT_DEFAULT__
#else
#error "DSM_STACK_MODE must be defined"
#endif

#ifdef DSM_PGTABLE_MODE_CXL
#define __MT_PGTABLE__ __MT_SHARED__
#elif defined DSM_PGTABLE_MODE_DRAM
#define __MT_PGTABLE__ __MT_PRIVATE__
#elif defined DSM_PGTABLE_MODE_MIXED
#define __MT_PGTABLE__ __MT_DEFAULT__
#else
#error "DSM_PGTABLE_MODE must be defined"
#endif

#ifdef DSM_THREADCTX_MODE_CXL
#define __MT_THREADCTX__ __MT_SHARED__
#elif defined DSM_THREADCTX_MODE_DRAM
#define __MT_THREADCTX__ __MT_PRIVATE__
#elif defined DSM_THREADCTX_MODE_MIXED
#define __MT_THREADCTX__ __MT_DEFAULT__
#else
#error "DSM_THREADCTX_MODE must be defined"
#endif

#ifdef DSM_OBJECT_MODE_CXL
#define __MT_OBJECT__ __MT_SHARED__
#elif defined DSM_OBJECT_MODE_DRAM
#define __MT_OBJECT__ __MT_PRIVATE__
#elif defined DSM_OBJECT_MODE_MIXED
#define __MT_OBJECT__ __MT_DEFAULT__
#else
#error "DSM_OBJECT_MODE must be defined"
#endif

#ifdef DSM_PAGE_MODE_CXL
#define __MT_PAGE__ __MT_SHARED__
#elif defined DSM_PAGE_MODE_DRAM
#define __MT_PAGE__ __MT_PRIVATE__
#elif defined DSM_PAGE_MODE_MIXED
#define __MT_PAGE__ __MT_DEFAULT__
#else
#error "DSM_PAGE_MODE must be defined"
#endif

#endif /* __DSM_MMCONFIG_H__ */
