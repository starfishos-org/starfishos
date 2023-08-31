add_library(pipe STATIC libpipe.c)
target_include_directories(pipe PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
