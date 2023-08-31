cmake_minimum_required(VERSION 3.14)

project(test_object C)
set(SOURCE_PATH ../../kernel/object)
set(OBJECT_DIR ${CMAKE_BINARY_DIR}/CMakeFiles/test_object.dir)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-arcs -ftest-coverage -pthread -g -DTEST_OBJECT")

if (ARCH)
else()
    set(ARCH "aarch64")
endif()
if (PLAT)
else()
    set(PLAT "raspi3")
endif()

file(GLOB KERN_SOURCE
    "${SOURCE_PATH}/thread.c"
    "${SOURCE_PATH}/cap_group.c"
    "${SOURCE_PATH}/capability.c")
set(SOURCES
    ${KERN_SOURCE}
    test_capability.c
    test_object.c
	stub.c
)

add_executable(test_object ${SOURCES})
include_directories(
	./include
    ../../kernel/include
	../../kernel/include/arch/${ARCH}
    ../../kernel/include/arch/${ARCH}/plat/${PLAT}
    ../include
    ../../kernel
    .
)

target_compile_options(test_object PRIVATE
	-fno-builtin-memset
	-fno-builtin-memcpy
	-fno-builtin-memmove
)

add_custom_target(
    lcov
    COMMAND lcov -d ${CMAKE_CURRENT_SOURCE_DIR} -z
    COMMAND lcov -d ${CMAKE_CURRENT_SOURCE_DIR} -b . --initial -c -o lcov.info
    COMMAND ${CMAKE_MAKE_PROGRAM} CTEST_OUTPUT_ON_FAILURE=1 test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
add_custom_command(
    TARGET lcov
    COMMAND lcov -d ${CMAKE_CURRENT_SOURCE_DIR} -c -o lcov.info
    COMMAND genhtml -o report --prefix=`pwd` lcov.info
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS test_object
)

enable_testing()
add_test(output_test ${CMAKE_CURRENT_BINARY_DIR}/test_object)
