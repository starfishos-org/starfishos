# chcore_enable_clang_tidy(
#     EXTRA_CHECKS -bugprone-implicit-widening-of-multiplication-result)
add_library(fat32_3rd_party OBJECT ffunicode.c ff.c)
# chcore_enable_clang_tidy()
add_executable(fat32.srv diskio.c ff_server.c ffsystem.c fat32_ops.c
                         fat32_utils.c)
target_link_libraries(fat32.srv PRIVATE fs_base fat32_3rd_party)
chcore_copy_all_targets_to_ramdisk()
