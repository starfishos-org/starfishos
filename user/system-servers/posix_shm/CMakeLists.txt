add_executable(posix_shm.srv main.c)

chcore_copy_all_targets_to_ramdisk()

target_link_options(posix_shm.srv PRIVATE -static)
