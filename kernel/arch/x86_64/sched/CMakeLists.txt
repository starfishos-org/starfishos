file(GLOB_RECURSE src "*.c" "*.S")
target_sources(${kernel_target} PRIVATE ${src})
