add_library(ffi STATIC ffi.c)
target_include_directories(ffi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
