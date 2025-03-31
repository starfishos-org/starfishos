add_subdirectory(json)

file(GLOB test_files "*.cpp" "*.c")

foreach(test_file ${test_files})
    get_filename_component(test_name ${test_file} NAME_WE)
    add_executable(${test_name}.bin ${test_file})
    if (CHCORE_USER_STATIC_APPS)
        target_link_options(${test_name}.bin PRIVATE -static)
        target_link_libraries(${test_name}.bin PRIVATE rpmalloc-static libc-static libstdc++-static)
    else()
        target_link_libraries(${test_name}.bin PRIVATE rpmalloc-shared)
    endif()
endforeach()

target_link_libraries(test_json.bin PRIVATE json11)
