add_executable(numa_bw_test.bin numa_bw_test.c)
target_compile_options(numa_bw_test.bin PRIVATE -O2)
if (CHCORE_USER_STATIC_APPS)
    target_link_options(numa_bw_test.bin PRIVATE -static)
    target_link_libraries(numa_bw_test.bin PRIVATE rpmalloc-static libc-static)
else()
    target_link_libraries(numa_bw_test.bin PRIVATE rpmalloc-shared)
    target_compile_options(numa_bw_test.bin PRIVATE -fPIE -fPIC)
    target_link_options(numa_bw_test.bin PRIVATE -pie)
endif()

if (CHCORE_BUILD_USER_MALLOC_TESTS)
    add_executable(perf_libc_malloc.bin perf_libc_malloc.c)
    target_compile_options(perf_libc_malloc.bin PRIVATE -O2)
    target_link_options(perf_libc_malloc.bin PRIVATE -pthread)
    if (CHCORE_USER_STATIC_APPS)
        target_link_options(perf_libc_malloc.bin PRIVATE -static)
        target_link_libraries(perf_libc_malloc.bin PRIVATE rpmalloc-static libc-static)
    else()
        target_link_libraries(perf_libc_malloc.bin PRIVATE rpmalloc-shared)
        target_compile_options(perf_libc_malloc.bin PRIVATE -fPIE -fPIC)
        target_link_options(perf_libc_malloc.bin PRIVATE -pie)
    endif()
    add_custom_command(
        TARGET perf_libc_malloc.bin POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/mem_alloc.txt
                ${build_ramdisk_dir}/mem_alloc.txt
        COMMENT "Copy mem_alloc.txt for perf_libc_malloc.bin random-size mode"
    )

    add_subdirectory(malloc_benckmark)
endif()

# add_executable(perf_mem.bin perf_mem.c)

# add_subdirectory(fxmark)
