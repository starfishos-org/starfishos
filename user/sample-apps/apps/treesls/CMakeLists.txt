# TreeSLS
# whole system tests
add_executable(shutdown.bin shutdown.c)
add_executable(idle.bin idle.c)

# checkpoint
add_executable(checkpoint.bin checkpoint.c)

# test applications
add_executable(test_crash_hello.bin test_crash_hello.c)
add_executable(test_crash_counter.bin test_crash_counter.c)

# adjust config
add_executable(set_poll_loop_time.bin set_poll_loop_time.c)
add_executable(set_dyn_hybrid_config.bin set_dyn_hybrid_config.c)

# performance test
add_executable(obj_ckpt_time.bin obj_ckpt_time.c)
# performance test: related apps
add_executable(simple_kvs.bin simple_kvs.c)
add_executable(mem_kvs.bin mem_kvs.c)
add_executable(ipc_kvs.bin ipc_kvs.c)
add_executable(fib.bin fib.c)
add_executable(ustress.bin ustress.c)
add_executable(get_free_mem_size.bin get_free_mem_size.c)
add_executable(test_heap.bin test_heap.c)
add_executable(test_fork.bin test_fork.c)

add_executable(test_merge_migration.bin test_merge_migration.c)
add_executable(test_migrate.bin test_migrate.c)
add_executable(test_hello.bin test_hello.c)
add_executable(persistent_hello.bin persistent_hello.c)
add_executable(nvm_hello.bin nvm_hello.c)
