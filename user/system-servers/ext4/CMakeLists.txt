file(GLOB _sources "*.c")
add_executable(ext4.srv ${_sources})
target_link_libraries(ext4.srv PRIVATE fs_base)

target_link_options(ext4.srv PRIVATE -static)
add_dependencies(ext4.srv libc)

chcore_copy_all_targets_to_ramdisk()
