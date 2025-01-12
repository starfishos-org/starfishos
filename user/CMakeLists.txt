cmake_minimum_required(VERSION 3.14)
project(ChCoreUser ASM C CXX)

set(custom_ramdisk_dir ${CHCORE_PROJECT_DIR}/ramdisk)
set(build_user_dir ${CMAKE_CURRENT_SOURCE_DIR}/build)
set(build_ramdisk_dir ${build_user_dir}/ramdisk)
set(build_tools_dir ${CHCORE_PROJECT_DIR}/tools/build)
set(build_demos_dir ${CMAKE_CURRENT_SOURCE_DIR}/demos)

# Target to re-create ramdisk, must be run before build user-level stuffs

file(REMOVE_RECURSE ${build_ramdisk_dir})
file(MAKE_DIRECTORY ${build_ramdisk_dir})

add_custom_target(
    ramdisk ALL
    WORKING_DIRECTORY ${build_ramdisk_dir}
    # install libc.so
    COMMAND cp ${CHCORE_MUSL_LIBC_INSTALL_DIR}/lib/libc.so .
    # install cpp library
    COMMAND cp ${CHCORE_MUSL_CROSS_MAKE_INSTALL_DIR}/x86_64-linux-musl/lib/libstdc++.so.6 .
    COMMAND cp ${CHCORE_MUSL_CROSS_MAKE_INSTALL_DIR}/x86_64-linux-musl/lib/libgcc_s.so.1 .
    COMMAND cp ${CHCORE_MUSL_CROSS_MAKE_INSTALL_DIR}/x86_64-linux-musl/lib/libgomp.so.1 .
    # install packages installed by ChPM
    COMMAND ${_copy_chpm_install_cmd}
    # libevent
    COMMAND cp /usr/libevent/lib/libevent-2.2.so.1.0.0 libevent-2.2.so.1
    # install custom ramdisk content
    COMMAND [ -d ${custom_ramdisk_dir} ] && cp -rL ${custom_ramdisk_dir}/* .
            || echo "No custom ramdisk found, skipping"
    # archive ramdisk into ramdisk.cpio
    COMMAND find . | cpio -o -H newc > ${CMAKE_CURRENT_BINARY_DIR}/ramdisk.cpio
    )
# FIXME: ramdisk should wait for all things being copied, add more to DEPENDS.

include(CommonTools)
include(UserTools)

set(_targets)
chcore_get_all_targets(_targets)
foreach(_target ${_targets})
  get_property(_installed GLOBAL PROPERTY ${_target}_INSTALLED)
  if(${_installed})
      continue()
  endif()
  get_target_property(_target_type ${_target} TYPE)
  if(${_target_type} STREQUAL SHARED_LIBRARY OR ${_target_type} STREQUAL
                                                EXECUTABLE)
      add_dependencies(ramdisk ${_target})
  endif()
endforeach()
unset(_targets)

chcore_dump_cmake_vars()

add_compile_options(-Wall)
add_compile_options(-Werror)
string(REPLACE "-DNDEBUG" "" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
string(REPLACE "-DNDEBUG" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")

include_directories(sys-include)

add_subdirectory(libraries)
add_subdirectory(system-servers)
add_subdirectory(sample-apps)
add_subdirectory(demos)

set(_clean_targets)
foreach(_target ${_targets})
  if(${_target} MATCHES "-clean$")
    list(APPEND _clean_targets ${_target})
  endif()
endforeach()
message(STATUS ${_clean_targets})
add_custom_target(custom-clean DEPENDS demos-clean ${_clean_targets})
