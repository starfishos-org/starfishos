set(LWIP_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(LWIP_INCLUDE_DIRS ports/chos/include src/include src/include/lwip)
set(LWIP_COMPILER_FLAGS -O3)
set(LWIP_DEFINITIONS -D_GNU_SOURCE)

include(src/Filelists.cmake)

# Disable warnings for LWIP core source files
chcore_target_remove_compile_options(lwipcore -Wall -Werror)
target_compile_options(lwipcore PRIVATE -w)

set(_chcore_port_sources ports/chos/sys_arch.c ports/chos/debug_flags.c)

add_executable(lwip.srv ${_chcore_port_sources} "main.c" "interface.c")
target_include_directories(lwip.srv PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(lwip.srv PRIVATE ${LWIP_COMPILER_FLAGS})
target_compile_definitions(lwip.srv PRIVATE ${LWIP_DEFINITIONS})
target_link_libraries(lwip.srv PRIVATE lwipcore)

add_executable(tcp_example.bin ${_chcore_port_sources} "tcpip_example.c")
target_include_directories(tcp_example.bin PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp_example.bin PRIVATE ${LWIP_COMPILER_FLAGS})
target_compile_definitions(tcp_example.bin PRIVATE ${LWIP_DEFINITIONS})
target_link_libraries(tcp_example.bin PRIVATE lwipcore)

target_link_options(lwip.srv PRIVATE -static)
add_dependencies(lwip.srv libc)

chcore_copy_all_targets_to_ramdisk()
