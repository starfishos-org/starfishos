file(GLOB _sources src/*.c)
add_library(tsm STATIC ${_sources})
target_include_directories(tsm PUBLIC include)
