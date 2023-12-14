add_link_options(-static)

add_executable(client.bin client.c)
add_executable(server.bin server.c)

add_executable(hello.bin hello.c)
add_executable(hello_libc.bin hello_libc.c)

add_executable(irq.bin irq.c)
add_executable(pipe.bin pipe.c)
add_executable(ttcp.bin ttcp.c)
add_executable(multicast.bin multicast.c)
add_executable(cat.bin cat.c)
add_executable(diff.bin diff.c)
add_executable(mount.bin mount.c)

add_executable(timerfd.bin timerfd.c)
add_executable(timerfd_epoll.bin timerfd_epoll.c)
add_executable(eventfd.bin eventfd.c)

add_executable(assert.bin assert.c)
target_compile_options(assert.bin PRIVATE -g3 -ggdb)

add_executable(udp_echo_server.bin udp_echo_server.c)
add_executable(tcp_echo_server.bin tcp_echo_server.c)

add_executable(pingpong.bin ping_pong.c)

# TreeSLS
add_subdirectory(treesls)

chcore_copy_all_targets_to_ramdisk()
