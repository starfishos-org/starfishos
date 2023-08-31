/* Binary include template. */

        .section .rodata
        .align 4
        .globl __binary_ramdisk_cpio_start
__binary_ramdisk_cpio_start:
        .incbin "${binary_path}"
__binary_ramdisk_cpio_end:
        .globl __binary_ramdisk_cpio_size
__binary_ramdisk_cpio_size:
        .quad __binary_ramdisk_cpio_end - __binary_ramdisk_cpio_start
