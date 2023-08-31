#pragma once

#define atomic_fetch_add_64(ptr, val) __sync_fetch_and_add(ptr, val)
#define atomic_fetch_sub_64(ptr, val) __sync_fetch_and_sub(ptr, val)
