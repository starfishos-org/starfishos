#pragma once

#include <assert.h>
#include <stdio.h>

#define PROCMGR_NOPRINT -1
#define PROCMGR_WARNING 0
#define PROCMGR_INFO    1
#define PROCMGR_DEBUG   2

#define PROCMGR_PRINT_LEVEL PROCMGR_NOPRINT

#define PREFIX "[procmgr]"

#if PROCMGR_PRINT_LEVEL >= PROCMGR_WARNING
#define warn(fmt, ...) printf(PREFIX " " fmt, ##__VA_ARGS__)
#else
#define warn(fmt, ...)
#endif

#if PROCMGR_PRINT_LEVEL >= PROCMGR_INFO
#define info(fmt, ...) printf(PREFIX " " fmt, ##__VA_ARGS__)
#else
#define info(fmt, ...)
#endif

#if PROCMGR_PRINT_LEVEL >= PROCMGR_DEBUG
#define debug(fmt, ...) \
	printf(PREFIX "<%s:%d>: " fmt, __func__, __LINE__, ##__VA_ARGS__)
#else
#define debug(fmt, ...)
#endif

#define error(fmt, ...) printf(PREFIX " " fmt, ##__VA_ARGS__)
