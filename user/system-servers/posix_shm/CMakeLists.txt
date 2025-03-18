add_executable(posix_shm.srv main.c)

target_link_options(posix_shm.srv PRIVATE -static)
add_dependencies(posix_shm.srv libc)

chcore_copy_all_targets_to_ramdisk()
