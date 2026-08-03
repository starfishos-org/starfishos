#pragma once

#include <stddef.h>

void dq_test_persist_range(const volatile void *addr, size_t len);
#define DQ_PERSIST_RANGE(addr, len) dq_test_persist_range((addr), (len))
