#pragma once

#include <lib/printk.h>

/* ANSI color codes for terminal output */
/* Note: For user space, use <chcore/ansi_color.h> instead */
#define ANSI_COLOR_RESET   "\033[0m"
#define ANSI_COLOR_BLACK   "\033[30m"
#define ANSI_COLOR_RED     "\033[31m"
#define ANSI_COLOR_GREEN   "\033[32m"
#define ANSI_COLOR_YELLOW  "\033[33m"
#define ANSI_COLOR_BLUE    "\033[34m"
#define ANSI_COLOR_MAGENTA "\033[35m"
#define ANSI_COLOR_CYAN    "\033[36m"
#define ANSI_COLOR_WHITE   "\033[37m"
#define ANSI_COLOR_BOLD    "\033[1m"

/* Bright colors (90-97) */
#define ANSI_COLOR_BRIGHT_BLACK   "\033[90m"
#define ANSI_COLOR_BRIGHT_RED     "\033[91m"
#define ANSI_COLOR_BRIGHT_GREEN   "\033[92m"
#define ANSI_COLOR_BRIGHT_YELLOW  "\033[93m"
#define ANSI_COLOR_BRIGHT_BLUE    "\033[94m"
#define ANSI_COLOR_BRIGHT_MAGENTA "\033[95m"  /* Bright purple/magenta */
#define ANSI_COLOR_BRIGHT_CYAN    "\033[96m"
#define ANSI_COLOR_BRIGHT_WHITE   "\033[97m"

#define WARNING 0
#define INFO    1
#define DEBUG   2

/* LOG_LEVEL is INFO by default */

#if LOG_LEVEL >= WARNING
#define kwarn(fmt, ...) printk(ANSI_COLOR_YELLOW "[WARN]" ANSI_COLOR_RESET " file:%s " fmt, __FILE__, ##__VA_ARGS__)
#define kwarn_once(fmt, ...)       \
    do {                           \
        static int __warned = 0;   \
        if (__warned)              \
            break;                 \
        __warned = 1;              \
        kwarn(fmt, ##__VA_ARGS__); \
    } while (0)

#else
#define kwarn(fmt, ...)
#define kwarn_once(fmt, ...)
#endif

#if LOG_LEVEL >= INFO
#define kinfo(fmt, ...) printk("[INFO] " fmt, ##__VA_ARGS__)
#else
#define kinfo(fmt, ...)
#endif

#if LOG_LEVEL >= DEBUG
#define kdebug(fmt, ...) printk("[DEBUG] " fmt, ##__VA_ARGS__)
#else
#define kdebug(fmt, ...)
#endif
