cmake_minimum_required(VERSION 3.14)
project(ChCoreUser ASM C CXX)

set(custom_ramdisk_dir ${CHCORE_PROJECT_DIR}/ramdisk)
set(build_user_dir ${CMAKE_CURRENT_SOURCE_DIR}/build)
set(build_ramdisk_dir ${build_user_dir}/ramdisk)
set(build_tools_dir ${CHCORE_PROJECT_DIR}/tools/build)
set(build_demos_dir ${CMAKE_CURRENT_SOURCE_DIR}/demos)
set(libc_install_dir ${CHCORE_MUSL_LIBC_INSTALL_DIR})

# Target to re-create ramdisk, must be run before build user-level stuffs

if (NOT EXISTS ${build_ramdisk_dir})
    file(MAKE_DIRECTORY ${build_ramdisk_dir})
endif()

add_library(libc-static STATIC IMPORTED)
set_property(TARGET libc-static PROPERTY IMPORTED_LOCATION ${CHCORE_MUSL_LIBC_INSTALL_DIR}/lib/libc.a)

add_library(libstdc++-static STATIC IMPORTED)
set_property(TARGET libstdc++-static PROPERTY IMPORTED_LOCATION ${CHCORE_MUSL_CROSS_MAKE_INSTALL_DIR}/x86_64-linux-musl/lib/libstdc++.a)

set(CMAKE_C_COMPILER ${libc_install_dir}/bin/musl-gcc)
set(CMAKE_CXX_COMPILER ${CHCORE_MUSL_CROSS_MAKE_INSTALL_DIR}/bin/x86_64-linux-musl-g++)

add_library(libc STATIC IMPORTED)
set_property(TARGET libc PROPERTY IMPORTED_LOCATION ${CHCORE_MUSL_LIBC_INSTALL_DIR}/lib/libc.a)

add_library(libstdc++ SHARED IMPORTED)
set_property(TARGET libstdc++ PROPERTY IMPORTED_LOCATION ${CHCORE_MUSL_CROSS_MAKE_INSTALL_DIR}/x86_64-linux-musl/lib/libstdc++.so.6)

add_custom_target(
    ramdisk ALL
    WORKING_DIRECTORY ${build_ramdisk_dir}
    # install libc.so
    COMMAND rsync -a ${CHCORE_MUSL_LIBC_INSTALL_DIR}/lib/libc.so ${build_ramdisk_dir}/libc.so
    # install cpp library
    COMMAND rsync -a ${CHCORE_MUSL_CROSS_MAKE_INSTALL_DIR}/x86_64-linux-musl/lib/libstdc++.so.6.0.28 ${build_ramdisk_dir}/libstdc++.so.6
    COMMAND rsync -a ${CHCORE_MUSL_CROSS_MAKE_INSTALL_DIR}/x86_64-linux-musl/lib/libgcc_s.so.1 ${build_ramdisk_dir}/libgcc_s.so.1
    COMMAND rsync -a ${CHCORE_MUSL_CROSS_MAKE_INSTALL_DIR}/x86_64-linux-musl/lib/libgomp.so.1.0.0 ${build_ramdisk_dir}/libgomp.so.1
    # install packages installed by ChPM
    COMMAND ${_copy_chpm_install_cmd}
    # libevent
    COMMAND rsync -a /usr/libevent/lib/libevent-2.2.so.1.0.0 ${build_ramdisk_dir}/libevent-2.2.so.1

    COMMAND rsync -a ${CMAKE_CURRENT_SOURCE_DIR}/script/* ${build_ramdisk_dir}/

    # install custom ramdisk content
    COMMAND [ -d ${custom_ramdisk_dir} ] && rsync -av ${custom_ramdisk_dir}/* . || echo "No custom ramdisk found, skipping"
    # archive ramdisk into ramdisk.cpio
    COMMAND find . | cpio -o -H newc > ${CMAKE_CURRENT_BINARY_DIR}/ramdisk.cpio
    COMMENT "Copying ramdisk files to ramdisk directory"
)
# FIXME: ramdisk should wait for all things being copied, add more to DEPENDS.

include(CommonTools)
include(UserTools)

chcore_dump_cmake_vars()

add_compile_options(-Wall)
# add_compile_options(-Werror)
string(REPLACE "-DNDEBUG" "" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
string(REPLACE "-DNDEBUG" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")

chcore_get_nproc(_nproc)

include_directories(sys-include)

add_subdirectory(libraries)
add_subdirectory(system-servers)
if (CHCORE_BUILD_SAMPLE_APPS)
	add_subdirectory(sample-apps)
endif()
if (CHCORE_BUILD_DEMO_APPS)
	add_subdirectory(demos)
endif()

set(_clean_targets)
foreach(_target ${_targets})
  if(${_target} MATCHES "-clean$")
    list(APPEND _clean_targets ${_target})
  endif()
endforeach()
message(STATUS "Clean targets: ${_clean_targets}")
add_custom_target(custom-clean DEPENDS libraries-clean demos-clean ${_clean_targets})
