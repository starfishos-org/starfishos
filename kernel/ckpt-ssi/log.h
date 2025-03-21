#ifndef __CFOR_LOG_H__
#define __CFOR_LOG_H__

#include <common/kprint.h>

#define CFORK_LOG_LEVEL_INFO 0
#define CFORK_LOG_LEVEL_ERR 1
#define CFORK_LOG_LEVEL_DEBUG 2


#define CFORK_LOG_LEVEL CFORK_LOG_LEVEL_DEBUG
#define CFORK_LOG_TAG "[CFORK]"

#if CFORK_LOG_LEVEL >= CFORK_LOG_LEVEL_INFO
#define CFORK_LOG_INFO(fmt, ...) printk(CFORK_LOG_TAG "[INFO] " fmt "\n", ##__VA_ARGS__)
#else
#define CFORK_LOG_INFO(fmt, ...)
#endif

#if CFORK_LOG_LEVEL >= CFORK_LOG_LEVEL_ERR
#define CFORK_LOG_ERR(fmt, ...) printk(CFORK_LOG_TAG "[ERROR] " fmt "\n", ##__VA_ARGS__)
#else
#define CFORK_LOG_ERR(fmt, ...)
#endif

#if CFORK_LOG_LEVEL >= CFORK_LOG_LEVEL_DEBUG
#define CFORK_LOG_DEBUG(fmt, ...) printk(CFORK_LOG_TAG "[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
#define CFORK_LOG_DEBUG(fmt, ...)
#endif

#endif