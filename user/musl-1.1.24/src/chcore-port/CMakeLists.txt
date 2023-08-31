cmake_minimum_required(VERSION 3.11)

add_library(common-lib STATIC
    "cpio.c"
    "elf.c"
    "ipc.c"
    "launch_kern.c"
    "liblauncher.c"
    "spawn.c"
    "syscall.c"
    "thread.c"
    "../lib/pmu.c"
    "../lib/syscall_arch.c"
    "../lib/spin_lock.c"
)

target_include_directories(common-lib
    PUBLIC
    "../include"
)
