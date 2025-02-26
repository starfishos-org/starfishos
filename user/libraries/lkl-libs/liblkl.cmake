# include config.cmake
include(config.cmake)

# Build Linux Kernel Library: liblkl.a
function(build_liblkl)
    add_custom_target(
        lkl-clean
        WORKING_DIRECTORY ${LKL_LINUX_DIR}
        COMMAND make clean
        COMMAND make clean -C tools/lkl
    )

    add_custom_target(
        lkl ALL
        WORKING_DIRECTORY ${LKL_LINUX_DIR}
        COMMAND make CC=${MUSL_CC} EXTRA_CFLAGS='-U__linux__ -D__CHCORE__' -C tools/lkl -j
        COMMENT "Build lkl finished"
        DEPENDS)

    add_custom_command(
        TARGET lkl
        POST_BUILD
        COMMAND cp ${LKL_LKL_DIR}/liblkl.a ${build_ramdisk_dir}/liblkl.a
        COMMENT "Copying llama.cpp files to ramdisk directory"
        DEPENDS lkl)
    add_dependencies(ramdisk lkl)
    add_dependencies(lkl_virtio_blk lkl)
endfunction()
