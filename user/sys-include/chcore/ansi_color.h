#pragma once

/* ANSI color codes for terminal output */
/* These can be used in both kernel and user space */

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
