add_subdirectory(json)

file(GLOB test_files "*.cpp" "*.c")

foreach(test_file ${test_files})
    get_filename_component(test_name ${test_file} NAME_WE)
    add_executable(${test_name}.bin ${test_file})
    target_link_options(${test_name}.bin PRIVATE -static)
    target_link_libraries(${test_name}.bin PRIVATE libc-static libstdc++-static)
endforeach()

target_link_libraries(test_json.bin PRIVATE json11)
