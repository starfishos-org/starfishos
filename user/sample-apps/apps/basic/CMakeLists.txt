file(GLOB UTILS_SOURCES "*.c")
# exclude assert.c
list(FILTER UTILS_SOURCES EXCLUDE REGEX "assert.c")

add_executable(assert.bin assert.c)
target_compile_options(assert.bin PRIVATE -g3 -ggdb)

foreach(SOURCE_FILE ${UTILS_SOURCES})
    get_filename_component(EXECUTABLE_NAME ${SOURCE_FILE} NAME_WE)
    add_executable(${EXECUTABLE_NAME}.bin ${SOURCE_FILE})
    if (CHCORE_USER_STATIC_APPS)
        target_link_options(${EXECUTABLE_NAME}.bin PRIVATE -static)
        target_link_libraries(${EXECUTABLE_NAME}.bin PRIVATE rpmalloc-static)
    else()
        target_link_libraries(${EXECUTABLE_NAME}.bin PRIVATE rpmalloc-shared)
    endif()
endforeach()
