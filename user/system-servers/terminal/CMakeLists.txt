add_subdirectory(libschrift)
add_subdirectory(libtsm)

add_executable(terminal.bin main.c terminal.c font.c keymap.c)
chcore_target_remove_link_options(terminal.bin -static)
target_link_libraries(terminal.bin PRIVATE schrift tsm wayland-client)
chcore_copy_all_targets_to_ramdisk()
