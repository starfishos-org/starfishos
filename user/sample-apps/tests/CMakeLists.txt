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

# add_executable(perf_libc_malloc.bin perf_libc_malloc.c)

# target_compile_options(perf_libc_malloc.bin PRIVATE -O2)

# if (CHCORE_USER_STATIC_APPS)
#   target_link_libraries(perf_libc_malloc.bin PRIVATE rpmalloc-static libc-static)
# else()
#   target_link_libraries(perf_libc_malloc.bin PRIVATE rpmalloc-shared)
# endif()

# add_custom_command(
#   TARGET perf_libc_malloc.bin
#   POST_BUILD
#   COMMAND cp ${CMAKE_CURRENT_SOURCE_DIR}/mem_alloc.txt ${build_ramdisk_dir}/
# )

# add_executable(perf_mem.bin perf_mem.c)

# add_subdirectory(malloc_benckmark)

# add_subdirectory(fxmark)
