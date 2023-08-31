#pragma once

#include <stdio.h>

#ifdef kdebug
#undef kdebug
#define kdebug(x, ...) printf(x, ##__VA_ARGS__)
#endif

#ifdef kinfo
#undef kinfo
#define kinfo(x, ...) printf(x, ##__VA_ARGS__)
#endif

#ifdef kwarn
#undef kwarn
#define kwarn(x, ...) printf(x, ##__VA_ARGS__)
#endif


#define kmalloc(sz) calloc(1, sz)
#define kfree free
#define kzalloc(sz) calloc(1, sz)
