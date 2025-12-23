add_library(polling-client-core STATIC polling_req.c)

target_include_directories(polling-client-core PUBLIC . ${src_system_servers_dir}/fs_base)
target_link_options(polling-client-core PRIVATE -static)
target_link_libraries(polling-client-core PRIVATE libc-static)

add_library(polling-server-core STATIC polling_resp.c)

target_include_directories(polling-server-core PUBLIC . ${src_system_servers_dir}/fs_base)
target_link_options(polling-server-core PRIVATE -static)
target_link_libraries(polling-server-core PRIVATE libc-static)

add_executable(polling.srv polling_server.c)

target_link_options(polling.srv PRIVATE -static)
target_link_libraries(polling.srv PRIVATE polling-server-core libc-static)

add_executable(polling_client.bin polling_client_test.c)

target_link_options(polling_client.bin PRIVATE -static)
target_link_libraries(polling_client.bin PRIVATE polling-client-core libc-static)

chcore_copy_all_targets_to_ramdisk()
