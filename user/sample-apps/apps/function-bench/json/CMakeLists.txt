add_library(json11 STATIC json11.cpp)
target_include_directories(json11 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_options(json11 PRIVATE -static)
target_link_libraries(json11 PRIVATE libc-static libstdc++-static)

add_custom_target(
    json_test ALL
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMAND rsync -a *.json ${build_ramdisk_dir}/
)
