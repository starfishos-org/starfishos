#pragma once

#include <common/types.h>

void x2apic_eoi(void);
void x2apic_init(void);
void x2apic_sipi(u32 hwid, u32 addr);

void x2apic_disable_timer(void);
void x2apic_enable_timer(void);
