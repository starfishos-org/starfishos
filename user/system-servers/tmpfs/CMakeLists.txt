chcore_enable_clang_tidy(
    EXTRA_CHECKS -performance-no-int-to-ptr,-clang-analyzer-deadcode.DeadStores)
message("CMAKE_C_CLANG_TIDY: ${CMAKE_C_CLANG_TIDY}")
set(binary_path ${build_user_dir}/ramdisk.cpio)
configure_file(incbin_ramdisk.tpl.S incbin_ramdisk.S)
unset(binary_path)

add_executable(tmpfs.srv tmpfs.c tmpfs_ops.c util.c)

add_dependencies(tmpfs.srv ramdisk)
target_sources(tmpfs.srv PRIVATE incbin_ramdisk.S)

target_link_libraries(tmpfs.srv PRIVATE fs_base)
