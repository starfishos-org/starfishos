cmake_minimum_required(VERSION 3.14)
project(ChCore)

set(_cmake_script_dir ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build/cmake)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${_cmake_script_dir}/Modules)

include(SubProject)
include(CommonTools)

chcore_dump_chcore_vars()

chcore_get_nproc(_nproc)

if(CHCORE_VERBOSE_BUILD)
    set(CMAKE_VERBOSE_MAKEFILE ON)
endif()

set(_common_args
    -DCMAKE_MODULE_PATH=${CMAKE_MODULE_PATH}
    -DCHCORE_PROJECT_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -DCMAKE_VERBOSE_MAKEFILE=${CMAKE_VERBOSE_MAKEFILE}
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)

# Construct cache args list for subprojects (kernel, user)
macro(chcore_config _config_name _config_type _default _description)
    if(NOT DEFINED ${_config_name})
        message(
            FATAL_ERROR
                "Do not run CMake command directly, use `./chbuild` instead")
    endif()
    list(APPEND _cache_args
         -D${_config_name}:${_config_type}=${${_config_name}})
endmacro()
include(${CMAKE_CURRENT_SOURCE_DIR}/config.cmake)

# --- LibC ---

set(_libc_source_dir ${CMAKE_CURRENT_SOURCE_DIR}/user/musl-1.1.24)
set(_libc_install_dir ${_libc_source_dir}/build)

# Main targets for Libc
chcore_add_subproject(
    libc
    SOURCE_DIR ${_libc_source_dir}
    INSTALL_DIR ${_libc_install_dir}
    BUILD_IN_SOURCE TRUE
    BUILD_ALWAYS TRUE
    CONFIGURE_COMMAND
        ./configure --prefix=<INSTALL_DIR> --syslibdir=<INSTALL_DIR>/lib
        $<$<BOOL:${CHCORE_USER_DEBUG}>:--enable-debug>
        CROSS_COMPILE=${CHCORE_CROSS_COMPILE} 
        $<$<BOOL:${CHCORE_ENABLE_FMAP}>:CFLAGS=-DCHCORE_ENABLE_FMAP>
        $<$<BOOL:${CHCORE_IPC_PERF_ENABLED}>:CFLAGS=-DIPC_PERF_ENABLED>
    BUILD_COMMAND make -j${_nproc}
    INSTALL_COMMAND make install)

# Clean target for LibC
add_custom_target(
    libc-clean
    WORKING_DIRECTORY ${_libc_source_dir}
    COMMAND make clean
    COMMAND /bin/rm -rf ${_libc_install_dir})

# --- User ---

set(_user_source_dir ${CMAKE_CURRENT_SOURCE_DIR}/user)
set(_user_build_dir ${_user_source_dir}/build)
set(_user_install_dir ${_user_build_dir})

add_custom_target(
    user-clean-incbin
    COMMAND
        [ -d ${_user_build_dir} ]
        && find ${_user_build_dir} -type f -name "incbin_*.S.*" | xargs rm -f
        || true)

# Main targets for user-level stuffs
chcore_add_subproject(
    user
    SOURCE_DIR ${_user_source_dir}
    BINARY_DIR ${_user_build_dir}
    INSTALL_DIR ${_user_install_dir}
    CMAKE_ARGS
        ${_common_args}
        -DCHCORE_MUSL_LIBC_INSTALL_DIR=${_libc_install_dir} # used by user.cmake toolchain to find `musl-gcc`
        -DCHCORE_MUSL_CROSS_MAKE_INSTALL_DIR=/home/musl-cross-make/install/
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DCMAKE_TOOLCHAIN_FILE=${_cmake_script_dir}/Toolchains/user.cmake
    CMAKE_CACHE_ARGS ${_cache_args}
    INSTALL_COMMAND echo "Nothing to install"
    DEPENDS libc user-clean-incbin
    BUILD_ALWAYS TRUE)

# Clean target for user-level stuffs
add_custom_target(
    user-clean
    COMMAND [ -f ${_user_build_dir}/CMakeCache.txt ] && ${CMAKE_COMMAND} --build
            ${_user_build_dir} --target custom-clean || true
    COMMAND /bin/rm -rf ${_user_build_dir})

# --- Ramdisk ---

if(CHCORE_CHPM_INSTALL_TO_RAMDISK AND CHCORE_CHPM_INSTALL_PREFIX)
    get_filename_component(_chpm_install_prefix ${CHCORE_CHPM_INSTALL_PREFIX}
                           REALPATH BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
    set(_copy_chpm_install_cmd
        "cp -rL `find ${_chpm_install_prefix} -mindepth 1 -maxdepth 1 -type d -not -name include -and -not -name 'chpm-*'` ."
    )
    string(REPLACE " " ";" _copy_chpm_install_cmd ${_copy_chpm_install_cmd})
else()
    set(_copy_chpm_install_cmd true)
endif()

# --- Kernel ---

set(_kernel_source_dir ${CMAKE_CURRENT_SOURCE_DIR}/kernel)
set(_kernel_build_dir ${_kernel_source_dir}/build)
set(_kernel_install_dir ${CMAKE_CURRENT_BINARY_DIR})

# Target to force re-including cpio binaries
add_custom_target(
    kernel-clean-incbin
    COMMAND
        [ -d ${_kernel_build_dir}/CMakeFiles ] && find
        ${_kernel_build_dir}/CMakeFiles -type f -name "incbin_*.S.*" | xargs rm
        -f || true)

# Main targets for kernel
chcore_add_subproject(
    kernel
    SOURCE_DIR ${_kernel_source_dir}
    BINARY_DIR ${_kernel_build_dir}
    INSTALL_DIR ${_kernel_install_dir}
    CMAKE_ARGS
        ${_common_args}
        -DCHCORE_USER_INSTALL_DIR=${_user_install_dir} # used by kernel/CMakeLists.txt to incbin cpio files
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DCMAKE_TOOLCHAIN_FILE=${_cmake_script_dir}/Toolchains/kernel.cmake
    CMAKE_CACHE_ARGS ${_cache_args}
    DEPENDS user kernel-clean-incbin
    BUILD_ALWAYS TRUE)

# Clean target for kernel
add_custom_target(
    kernel-clean
    COMMAND /bin/rm -rf ${_kernel_build_dir}
    COMMAND [ -f ${_kernel_install_dir}/install_manifest.txt ] && cat
            ${_kernel_install_dir}/install_manifest.txt | xargs rm -rf || true)

# --- Clean All ---

add_custom_target(
    clean-all
    COMMAND ${CMAKE_COMMAND} --build . --target kernel-clean
    COMMAND ${CMAKE_COMMAND} --build . --target user-clean
    COMMAND ${CMAKE_COMMAND} --build . --target libc-clean)

# --- Update Submodules (always run in local env) ---

add_custom_target(update-submodules)

macro(_update_submodule _target _module_path)
    add_custom_target(
        ${_target}
        COMMAND echo "Updating ${_module_path}..."
        COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} submodule update --init
                --recursive --depth=1 ${_module_path})
    add_dependencies(update-submodules ${_target})
endmacro()

if(CHCORE_DRIVER_FWK_LINUX)
    _update_submodule(update-linux-port user/system-servers/drivers/linux-port)
endif()
if(CHCORE_DRIVER_FWK_CIRCLE)
    _update_submodule(update-circle user/system-servers/drivers/raspi/circle)
endif()
if(CHCORE_APP_VMM)
    _update_submodule(update-vm-images user/vmm/vm-images)
endif()
if(CHCORE_CROSS_COMPILE MATCHES "riscv64")
    _update_submodule(opensbi kernel/arch/riscv64/boot/opensbi)
endif()
