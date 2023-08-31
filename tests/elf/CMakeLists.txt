cmake_minimum_required(VERSION 3.14)

project(test_elf C)
set(SOURCE_PATH ../../../kernel/lib)
set(OBJECT_DIR ${CMAKE_BINARY_DIR}/CMakeFiles/test_elf.dir)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-arcs -ftest-coverage -g")

set(SOURCES
    test_elf.c
)

add_executable(test_elf ${SOURCES})
include_directories(
    ../../kernel/include
    ../include
)

add_custom_target(
    lcov
    COMMAND lcov -d ${CMAKE_CURRENT_SOURCE_DIR} -z
    COMMAND lcov -d ${CMAKE_CURRENT_SOURCE_DIR} -b . --initial -c -o lcov.info
    COMMAND CTEST_OUTPUT_ON_FAILURE=1 ${CMAKE_MAKE_PROGRAM} test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
add_custom_command(
    TARGET lcov
    COMMAND lcov -d ${CMAKE_CURRENT_SOURCE_DIR} -c -o lcov.info
    COMMAND genhtml -o report --prefix=`pwd` lcov.info
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS test_aarch64_page_table
)

file(COPY hello.out DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

enable_testing()
add_test(test_elf ${CMAKE_CURRENT_BINARY_DIR}/test_elf)
