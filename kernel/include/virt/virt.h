#pragma once

#include <common/util.h>

extern void configure_stage2_mmu(void);
void primary_virt_init(void);
extern void flush_data_cache(void);
void secondary_virt_init(void);
