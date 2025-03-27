add_library(json11 STATIC json11.cpp)
target_include_directories(json11 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_options(json11 PRIVATE -static)
target_link_libraries(json11 PRIVATE libc-static)

add_executable(json_test.bin test_json.cpp)
target_link_options(json_test.bin PRIVATE -static)
target_link_libraries(json_test.bin PRIVATE libc-static json11)

add_custom_target(
    json_test ALL
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMAND rsync -a *.json ${build_ramdisk_dir}/
)
