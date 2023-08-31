target_sources(${kernel_target} PRIVATE ipi.c irq_entry.c irq_entry.S pgfault.c
                                        syscall.c x2apic.c)
