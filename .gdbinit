# Establish gdb connection through the port specified in build/gdb-port.
# Provide two commands: add-symbol-file-off and prepare-load-lib.
source ./scripts/gdb/gdb.py

set substitute-path /chos/ ./

add-symbol-file-off user/musl-1.1.24/build/lib/libc.so 0x400000000000
add-symbol-file-off build/kernel.img
add-symbol-file user/build/ramdisk/ycsbc
add-symbol-file-off user/build/ramdisk/libstdc++.so.6 0x300000058000
