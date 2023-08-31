#pragma once

#include <chcore/type.h>

#ifdef __cplusplus
extern "C" {
#endif

int create_thread(void *(*func)(void *), u64 arg, u32 prio, u64 tls);

#ifdef __cplusplus
}
#endif
