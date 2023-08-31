#pragma once
#include <common/macro.h>

#define PAGE_SIZE (0x1000)

#define phys_to_virt(x) ({ BUG("not implemented\n"); 0ull; })
#define virt_to_phys(x) ({ BUG("not implemented\n"); 0ull; })
