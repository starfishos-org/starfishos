# chcore_enable_clang_tidy()
add_executable(chcore_shell.bin chcore_shell.c job_control.c buildin_cmd.c
                                handle_input.c)

target_link_options(chcore_shell.bin PRIVATE -static)
add_dependencies(chcore_shell.bin libc-static)

chcore_copy_all_targets_to_ramdisk()
