# include config.cmake
include(config.cmake)
include(liblkl.cmake)

# Build Linux Kernel Library: liblkl.a
if(CHCORE_LIBRARY_LKL_LIB)
    build_liblkl()
endif()

# Build Linux Kernel Library Tools
include_directories(${LKL_LKL_DIR}/include)
add_compile_definitions(__CHCORE__)

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
add_dependencies(lkl_virtio_blk lkl)

chcore_copy_all_targets_to_ramdisk()
