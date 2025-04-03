#pragma once

#include <common/types.h>

static inline u64 calculate_checksum(void *data, size_t size)
{
    u64 checksum = 0;
    for (int i = 0; i < size; i++) {
        checksum += ((u8 *)data)[i];
    }
    return checksum;
}

static inline bool verify_checksum(void *data, size_t size, u64 checksum)
{
    return calculate_checksum(data, size) == checksum;
}
