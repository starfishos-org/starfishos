file(GLOB _sources "*.c")
add_executable(ext4.srv ${_sources})
target_link_libraries(ext4.srv PRIVATE fs_base)
chcore_copy_all_targets_to_ramdisk()
