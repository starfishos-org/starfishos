file(GLOB _sources "*.c")
add_executable(fsm.srv ${_sources})

target_link_options(fsm.srv PRIVATE -static)
target_link_libraries(fsm.srv PRIVATE libc-static)