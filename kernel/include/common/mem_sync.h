#pragma once

#define FLUSH(addr) asm volatile("clwb (%0)" ::"r"(addr) : "memory")
#define FENCE       asm volatile("sfence" ::: "memory")

#define FLUSH_RANGE(addr, len) do {                                         \
    unsigned long __flush_p = (unsigned long)(addr) & ~63UL;                \
    unsigned long __flush_end =                                             \
            ((unsigned long)(addr) + (unsigned long)(len) + 63UL) & ~63UL;  \
    for (; __flush_p < __flush_end; __flush_p += 64)                        \
        FLUSH((void *)__flush_p);                                           \
    FENCE;                                                                  \
} while (0)
