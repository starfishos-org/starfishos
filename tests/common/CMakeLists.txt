cmake_minimum_required(VERSION 3.14)

project(test_common)
set(SOURCE_PATH ../../../kernel/lib)
set(OBJECT_DIR ${CMAKE_BINARY_DIR}/CMakeFiles/test_common.dir)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-arcs -ftest-coverage -g")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage -g")

set(SOURCES
    test_list.c
)

add_executable(test_list ${SOURCES})
add_executable(test_hashtable test_hashtable.c)
add_executable(test_radix test_radix.c)
add_executable(test_user_radix test_user_radix.c)
add_executable(test_rbtree test_rbtree.c)
add_executable(test_user_ringbuffer test_user_ringbuffer.c ring_buffer.c)
add_executable(test_user_rbtree test_user_rbtree.cpp)

target_include_directories(test_list
    PRIVATE ../../kernel/include
    PRIVATE ../include
)

target_include_directories(test_hashtable
    PRIVATE ../../kernel/include
    PRIVATE ../include
)

target_include_directories(test_radix
    PRIVATE ../../kernel/include
    PRIVATE ../include
)

target_include_directories(test_rbtree
    PRIVATE ../../kernel/include
    PRIVATE ../include
)

target_include_directories(test_user_radix
    PRIVATE ../include
)

target_include_directories(test_user_rbtree
    PRIVATE ../include
)

target_include_directories(test_user_ringbuffer
    PRIVATE ../include
)

target_link_libraries(test_user_ringbuffer pthread)

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
    DEPENDS test_list
)

enable_testing()
add_test(test_list ${CMAKE_CURRENT_BINARY_DIR}/test_list)
add_test(test_hashtable ${CMAKE_CURRENT_BINARY_DIR}/test_hashtable)
add_test(test_radix ${CMAKE_CURRENT_BINARY_DIR}/test_radix)
add_test(test_user_radix ${CMAKE_CURRENT_BINARY_DIR}/test_user_radix)
add_test(test_rbtree ${CMAKE_CURRENT_BINARY_DIR}/test_rbtree)
add_test(test_user_ringbuffer ${CMAKE_CURRENT_BINARY_DIR}/test_user_ringbuffer)
add_test(test_user_rbtree ${CMAKE_CURRENT_BINARY_DIR}/test_user_rbtree)