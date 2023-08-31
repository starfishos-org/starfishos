/* Kernel binary include template. */

        .section .rodata
        .align 4
        .globl binary_${binary_name}_start
binary_${binary_name}_start:
        .incbin "${binary_path}"
binary_${binary_name}_end:
        .globl binary_${binary_name}_size
binary_${binary_name}_size:
        .quad binary_${binary_name}_end - binary_${binary_name}_start
