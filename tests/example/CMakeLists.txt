cmake_minimum_required(VERSION 3.14)

set(PROJECT foo)

project(test_${PROJECT} C)
#set(SOURCE_PATH ../../kernel/${PROJECT})
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-arcs -ftest-coverage -g")

set(SOURCES
    #${SOURCE_PATH}/foo.c
    #${SOURCE_PATH}/bar.c
    foo.c
    test_foo.c
)

add_executable(test_${PROJECT} ${SOURCES})
include_directories(
    #../../kernel/{PROJECT}/include
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
    DEPENDS test_${PROJECT}
)

enable_testing()
add_test(output_test ${CMAKE_CURRENT_BINARY_DIR}/test_${PROJECT})
