#pragma once

#include <mm/mm.h>
#include <common/types.h>

int copy_from_user(char *kbuf, char *ubuf, size_t size);
int copy_to_user(char *ubuf, char *kbuf, size_t size);

static inline int check_user_addr_range(vaddr_t start, size_t len)
{
        if ((start + len) >= KBASE)
                return -1;
        return 0;
}
