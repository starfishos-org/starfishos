# Establish gdb connection through the port specified in build/gdb-port.
# Provide two commands: add-symbol-file-off and prepare-load-lib.
source ./scripts/gdb/gdb.py

set debuginfod enabled on

# add-symbol-file-off user/musl-1.1.24/build/lib/libc.so 0x400000000000
file build/kernel.img
# add-symbol-file-off user/build/ramdisk/matrix_multiply_cxl.bin 0x300000003000
