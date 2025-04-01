# chcore_enable_clang_tidy()
add_executable(procmgr.srv proc_node.c procmgr.c recycle.c shell_msg_handler.c
                           srvmgr.c)
target_link_options(procmgr.srv PRIVATE -static)
add_dependencies(procmgr.srv libc)

macro(_procmgr_incbin _target_name _binary_name _binary_path)
    set(_binary_name ${_binary_name})
    set(_binary_path ${_binary_path})
    configure_file(incbin_basic_srv.tpl.S incbin_${_binary_name}.S)
    target_sources(procmgr.srv PRIVATE incbin_${_binary_name}.S)
    add_dependencies(procmgr.srv ${_target_name})
    unset(_binary_name)
    unset(_binary_path)
endmacro()

_procmgr_incbin(fsm.srv fsm_elf ${build_user_dir}/system-servers/fsm/fsm.srv)
_procmgr_incbin(tmpfs.srv tmpfs_elf ${build_user_dir}/system-servers/tmpfs/tmpfs.srv)

target_link_options(procmgr.srv PRIVATE -T
                    ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld)

add_custom_target(
    create-procmgr-bin ALL
    COMMAND echo "build procmgr binary..."
    COMMAND read_procmgr_elf_tool ${CMAKE_CURRENT_BINARY_DIR}/procmgr.srv
    COMMAND chmod 777 elf_info.temp
    COMMAND cat elf_info.temp procmgr.bin > procmgr
    COMMAND rm -f ${CMAKE_CURRENT_BINARY_DIR}/elf_info.temp
    COMMAND echo "build procmgr success!"
    DEPENDS procmgr.srv procmgr.bin)

chcore_objcopy_binary(procmgr.srv procmgr.bin)

target_link_libraries(procmgr.srv PRIVATE libc-static)
