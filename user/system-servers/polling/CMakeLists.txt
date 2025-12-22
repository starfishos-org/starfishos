add_library(polling STATIC polling.c)

target_include_directories(polling PUBLIC . ${src_system_servers_dir}/fs_base)
target_link_options(polling PRIVATE -static)
target_link_libraries(polling PRIVATE libc-static)

add_executable(polling.srv polling_server.c)

target_link_options(polling.srv PRIVATE -static)
target_link_libraries(polling.srv PRIVATE polling libc-static)

add_executable(polling_client.bin polling_client_test.c)

target_link_options(polling_client.bin PRIVATE -static)
target_link_libraries(polling_client.bin PRIVATE polling libc-static)

add_executable(tmp.bin tmp.c)

target_link_options(tmp.bin PRIVATE -static)
target_link_libraries(tmp.bin PRIVATE polling libc-static)


chcore_copy_all_targets_to_ramdisk()
