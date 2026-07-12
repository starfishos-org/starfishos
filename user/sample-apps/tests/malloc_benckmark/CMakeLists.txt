add_executable(malloc_benchmark.bin main.c benchmark.c thread.c timer.c)

if (CHCORE_USER_STATIC_APPS)
  target_link_options(malloc_benchmark.bin PRIVATE -static)
  target_link_libraries(malloc_benchmark.bin PRIVATE rpmalloc-static libc-static libstdc++-static)
else()
  target_link_libraries(malloc_benchmark.bin PRIVATE rpmalloc-shared)
  target_compile_options(malloc_benchmark.bin PRIVATE -fPIE -fPIC)
  target_link_options(malloc_benchmark.bin PRIVATE -pie)
endif()

add_custom_target(malloc_benchmark-copy ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:malloc_benchmark.bin>
            ${build_ramdisk_dir}/malloc_benchmark.bin
    DEPENDS malloc_benchmark.bin
    COMMENT "Copy malloc_benchmark.bin to ramdisk"
)
add_dependencies(ramdisk malloc_benchmark-copy)
