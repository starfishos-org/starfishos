add_link_options(-static)

add_executable(perf_libc_malloc.bin perf_libc_malloc.c)

target_compile_options(perf_libc_malloc.bin PRIVATE -O2)

add_custom_command(
  TARGET perf_libc_malloc.bin
  POST_BUILD
  COMMAND cp ${CMAKE_CURRENT_SOURCE_DIR}/mem_alloc.txt ${build_ramdisk_dir}/
)

add_executable(perf_mem.bin perf_mem.c)

add_subdirectory(malloc_benckmark)

add_executable(simple_test.bin simple_test.c)
