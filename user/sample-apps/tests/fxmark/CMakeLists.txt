file(GLOB_RECURSE fxmark_srcs ${CMAKE_CURRENT_SOURCE_DIR}/*.c)

add_executable(fxmark ${fxmark_srcs})
