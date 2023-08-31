/* Binary include template. */

        .section .rodata
        .align 4
        .globl __binary_${_binary_name}_start
__binary_${_binary_name}_start:
        .incbin "${_binary_path}"
__binary_${_binary_name}_end:
        .globl __binary_${_binary_name}_size
__binary_${_binary_name}_size:
        .quad __binary_${_binary_name}_end - __binary_${_binary_name}_start
