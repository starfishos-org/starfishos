target_sources(${kernel_target} PRIVATE acpi/acpi.c acpi/madt.c acpi/nfit.c)
target_sources(${kernel_target} PRIVATE console/console.c console/fb.c)
target_sources(${kernel_target} PRIVATE multiboot2/multiboot2.c)
target_sources(${kernel_target} PRIVATE uart/uart.c)
