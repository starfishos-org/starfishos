#define FLUSH(addr) asm volatile("clwb (%0)" ::"r"(addr))
#define FENCE       asm volatile("sfence" ::: "memory")
