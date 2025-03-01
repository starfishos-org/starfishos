add_link_options(-static)

file(GLOB UTILS_SOURCES "*.c")

foreach(SOURCE_FILE ${UTILS_SOURCES})
    get_filename_component(EXECUTABLE_NAME ${SOURCE_FILE} NAME_WE)
    add_executable(${EXECUTABLE_NAME} ${SOURCE_FILE})
endforeach()
