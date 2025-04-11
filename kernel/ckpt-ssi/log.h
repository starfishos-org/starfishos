#ifndef __CFOR_LOG_H__
#define __CFOR_LOG_H__

#include <common/kprint.h>

#define CFORK_LOG_LEVEL_PERF 0
#define CFORK_LOG_LEVEL_INFO 1
#define CFORK_LOG_LEVEL_ERR 2
#define CFORK_LOG_LEVEL_WARN 3
#define CFORK_LOG_LEVEL_DEBUG 4


#define CFORK_LOG_LEVEL CFORK_LOG_LEVEL_PERF
#define CFORK_LOG_TAG "[CFORK]"

#if CFORK_LOG_LEVEL >= CFORK_LOG_LEVEL_INFO
#define CFORK_LOG_INFO(fmt, ...) printk(CFORK_LOG_TAG "[INFO] " fmt, ##__VA_ARGS__)
#else
#define CFORK_LOG_INFO(fmt, ...)
#endif

#if CFORK_LOG_LEVEL >= CFORK_LOG_LEVEL_WARN
#define CFORK_LOG_WARN(fmt, ...) printk(CFORK_LOG_TAG "[WARN] " fmt, ##__VA_ARGS__)
#else
#define CFORK_LOG_WARN(fmt, ...)
#endif

#if CFORK_LOG_LEVEL >= CFORK_LOG_LEVEL_ERR
#define CFORK_LOG_ERR(fmt, ...) printk(CFORK_LOG_TAG "[ERROR] " fmt, ##__VA_ARGS__)
#else
#define CFORK_LOG_ERR(fmt, ...)
#endif

#if CFORK_LOG_LEVEL >= CFORK_LOG_LEVEL_DEBUG
#define CFORK_LOG_DEBUG(fmt, ...) printk(CFORK_LOG_TAG "[DEBUG] " fmt, ##__VA_ARGS__)
#else
#define CFORK_LOG_DEBUG(fmt, ...)
#endif

#endif