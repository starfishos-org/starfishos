# Establish gdb connection through the port specified in build/gdb-port.
# Provide two commands: add-symbol-file-off and prepare-load-lib.
source ./scripts/gdb/gdb.py

set debuginfod enabled on

file build/kernel.img
add-symbol-file user/build/ramdisk/libc.so 0x400000000000
