set(binary_path ${build_user_dir}/ramdisk.cpio)
configure_file(incbin_ramdisk.tpl.S incbin_ramdisk.S)
unset(binary_path)

add_executable(tmpfs.srv tmpfs.c tmpfs_ops.c util.c cxlfs.c plog.c tmpfs_polling.c)
set_target_properties(tmpfs.srv PROPERTIES OUTPUT_NAME cxlfs.srv)
# chcore/cpio.h exposes its legacy tentative g_files definition to each TU.
target_compile_options(tmpfs.srv PRIVATE -fcommon)

add_dependencies(tmpfs.srv ramdisk)
target_sources(tmpfs.srv PRIVATE incbin_ramdisk.S)

target_link_libraries(tmpfs.srv PRIVATE fs_base polling-client-core)

target_include_directories(tmpfs.srv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../polling)

target_link_options(tmpfs.srv PRIVATE -static)

target_link_libraries(tmpfs.srv PRIVATE libc-static rpmalloc-static)

if(CHCORE_IPC_PERF_ENABLED)
    target_compile_definitions(tmpfs.srv PRIVATE IPC_PERF_ENABLED)
endif()
