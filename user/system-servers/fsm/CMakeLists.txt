file(GLOB _sources "*.c")
add_executable(fsm.srv ${_sources})

target_link_options(fsm.srv PRIVATE -static)

if(CHCORE_IPC_PERF_ENABLED)
    target_compile_definitions(fsm.srv PRIVATE IPC_PERF_ENABLED)
endif()

add_dependencies(fsm.srv libc)