#ifndef __CKPT_LOG_H__
#define __CKPT_LOG_H__

#include <common/kprint.h>

#define CKPT_LOG_LEVEL_PERF 0
#define CKPT_LOG_LEVEL_INFO 1
#define CKPT_LOG_LEVEL_ERR 2
#define CKPT_LOG_LEVEL_WARN 3
#define CKPT_LOG_LEVEL_DEBUG 4


#define CKPT_LOG_LEVEL CKPT_LOG_LEVEL_PERF
#define CKPT_LOG_TAG "[CKPT]"

#if CKPT_LOG_LEVEL >= CKPT_LOG_LEVEL_INFO
#define CKPT_LOG_INFO(fmt, ...) printk(CKPT_LOG_TAG "[INFO] " fmt, ##__VA_ARGS__)
#else
#define CKPT_LOG_INFO(fmt, ...)
#endif

#if CKPT_LOG_LEVEL >= CKPT_LOG_LEVEL_WARN
#define CKPT_LOG_WARN(fmt, ...) printk(CKPT_LOG_TAG "[WARN] " fmt, ##__VA_ARGS__)
#else
#define CKPT_LOG_WARN(fmt, ...)
#endif

#if CKPT_LOG_LEVEL >= CKPT_LOG_LEVEL_ERR
#define CKPT_LOG_ERR(fmt, ...) printk(CKPT_LOG_TAG "[ERROR] " fmt, ##__VA_ARGS__)
#else
#define CKPT_LOG_ERR(fmt, ...)
#endif

#if CKPT_LOG_LEVEL >= CKPT_LOG_LEVEL_DEBUG
#define CKPT_LOG_DEBUG(fmt, ...) printk(CKPT_LOG_TAG "[DEBUG] " fmt, ##__VA_ARGS__)
#else
#define CKPT_LOG_DEBUG(fmt, ...)
#endif

#endif