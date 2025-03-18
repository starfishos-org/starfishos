file(GLOB _sources "*.c")
add_executable(fsm.srv ${_sources})

target_link_options(fsm.srv PRIVATE -static)
add_dependencies(fsm.srv libc)