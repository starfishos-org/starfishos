file(GLOB _dm_source_files "*.c")
add_library(dm SHARED ${_dm_source_files})
target_include_directories(dm PUBLIC include)
