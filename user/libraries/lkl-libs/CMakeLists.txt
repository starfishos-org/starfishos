# include config.cmake
include(config.cmake)

# Build Linux Kernel Library: liblkl.a
function(build_liblkl)
    add_custom_target(
        lkl-clean
        WORKING_DIRECTORY ${LKL_LINUX_DIR}
        COMMAND make clean
        COMMAND make clean -C tools/lkl
        COMMENT "Cleaning lkl library finished"
    )

    add_custom_target(
        lkl ALL
        WORKING_DIRECTORY ${LKL_LINUX_DIR}
        COMMAND make CC=${MUSL_CC} EXTRA_CFLAGS='-U__linux__ -D__CHCORE__' -C tools/lkl -j
        COMMENT "Build lkl finished"
        DEPENDS)
    add_dependencies(ramdisk lkl)
    list(APPEND _libraries_clean_targets lkl-clean)
endfunction()

# Build Linux Kernel Library: liblkl.a
if(CHCORE_LIBRARY_LKL_LIB)
    build_liblkl()
endif()

# Build Linux Kernel Library Tools
include_directories(${LKL_LKL_DIR}/include)
add_compile_options(-D__CHCORE__)

message(STATUS "LKL_LKL_DIR is set to: ${LKL_LKL_DIR}")
find_library(LKL_LIBRARY
    NAMES lkl
    PATHS ${LKL_LKL_DIR}
    NO_DEFAULT_PATH
)

if(NOT LKL_LIBRARY)
    message(FATAL_ERROR "LKL library not found")
endif()

file(GLOB common_src cla.c log.c)

add_executable(lkl_virtio_blk ${common_src} tools-disk-vfio-pci.c)
target_link_libraries(lkl_virtio_blk PRIVATE ${LKL_LIBRARY})
if(CHCORE_LIBRARY_LKL_LIB)
    add_dependencies(lkl_virtio_blk lkl)
endif()

chcore_copy_all_targets_to_ramdisk()
