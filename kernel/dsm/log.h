#ifndef __DSM_TIERING_LOG_H__
#define __DSM_TIERING_LOG_H__

#include <common/kprint.h>

#define DSM_TIER_LOG_LEVEL_INFO 0
#define DSM_TIER_LOG_LEVEL_ERR 1
#define DSM_TIER_LOG_LEVEL_WARN 2
#define DSM_TIER_LOG_LEVEL_DEBUG 3


#define DSM_TIER_LOG_LEVEL DSM_TIER_LOG_LEVEL_DEBUG
#define DSM_TIER_LOG_TAG "[DSM TIER]"

#if DSM_TIER_LOG_LEVEL >= DSM_TIER_LOG_LEVEL_INFO
#define DSM_TIER_LOG_INFO(fmt, ...) printk(DSM_TIER_LOG_TAG "[INFO] " fmt, ##__VA_ARGS__)
#else
#define DSM_TIER_LOG_INFO(fmt, ...)
#endif

#if DSM_TIER_LOG_LEVEL >= DSM_TIER_LOG_LEVEL_WARN
#define DSM_TIER_LOG_WARN(fmt, ...) printk(DSM_TIER_LOG_TAG "[WARN] " fmt, ##__VA_ARGS__)
#else
#define DSM_TIER_LOG_WARN(fmt, ...)
#endif

#if DSM_TIER_LOG_LEVEL >= DSM_TIER_LOG_LEVEL_ERR
#define DSM_TIER_LOG_ERR(fmt, ...) printk(DSM_TIER_LOG_TAG "[ERROR] " fmt, ##__VA_ARGS__)
#else
#define DSM_TIER_LOG_ERR(fmt, ...)
#endif

#if DSM_TIER_LOG_LEVEL >= DSM_TIER_LOG_LEVEL_DEBUG
#define DSM_TIER_LOG_DEBUG(fmt, ...) printk(DSM_TIER_LOG_TAG "[DEBUG] " fmt, ##__VA_ARGS__)
#else
#define DSM_TIER_LOG_DEBUG(fmt, ...)
#endif

#endif