add_library(fs_base STATIC fs_page_cache.c fs_page_fault.c fs_vnode.c fs_wrapper_ops.c
                           fs_wrapper.c)
target_include_directories(fs_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_options(fs_base PRIVATE -static)
add_dependencies(fs_base libc)

if(CHCORE_IPC_PERF_ENABLED)
    target_compile_definitions(fs_base PRIVATE IPC_PERF_ENABLED)
endif()

chcore_copy_all_targets_to_ramdisk()
