OUTPUT_FORMAT("elf64-x86-64", "elf64-x86-64", "elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

ENTRY(_start)

SECTIONS
{
    /* for suppressing relocation complaints in debug building */
    .debug_info 0 : {
        *(.debug_info)
        *(.debug_str)
        *(.debug*)
    }

    /* Offset 1M is reserved for BIOS code */
    . = 0xFFFFFFFFC0100000;
    PROVIDE(text = .);

    .text : AT(0x100000) {
        ${init_objects}
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    . = ALIGN(0x1000);
    .data : {
        *(.data*)
    }
    PROVIDE(edata = .);

    .bss : {
        *(.bss*)
    }
    . = ALIGN(0x1000);

    . = ALIGN(0x200000);
    PROVIDE(end = .);
    img_end = .;
}
