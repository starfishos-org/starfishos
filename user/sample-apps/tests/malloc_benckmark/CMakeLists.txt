add_executable(malloc_benchmark.bin main.c benchmark.c thread.c timer.c)

target_link_libraries(malloc_benchmark.bin PRIVATE m pthread)
