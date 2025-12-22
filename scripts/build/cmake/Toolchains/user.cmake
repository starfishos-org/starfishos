# CMake toolchain for building ChCore user-level libs and apps.

if(NOT DEFINED CHCORE_PROJECT_DIR)
    message(FATAL_ERROR "CHCORE_PROJECT_DIR is not defined")
else()
    message(STATUS "CHCORE_PROJECT_DIR: ${CHCORE_PROJECT_DIR}")
endif()

if(NOT DEFINED CHCORE_MUSL_LIBC_INSTALL_DIR)
    message(FATAL_ERROR "CHCORE_MUSL_LIBC_INSTALL_DIR is not defined")
else()
    message(
        STATUS "CHCORE_MUSL_LIBC_INSTALL_DIR: ${CHCORE_MUSL_LIBC_INSTALL_DIR}")
endif()

# Set toolchain executables
set(CMAKE_ASM_COMPILER ${CHCORE_MUSL_LIBC_INSTALL_DIR}/bin/musl-gcc)
set(CMAKE_C_COMPILER ${CHCORE_MUSL_LIBC_INSTALL_DIR}/bin/musl-gcc)
set(CMAKE_CXX_COMPILER ${CHCORE_MUSL_LIBC_INSTALL_DIR}/bin/musl-gcc)
set(CMAKE_AR ${CHCORE_MUSL_LIBC_INSTALL_DIR}/bin/musl-ar)
set(CMAKE_NM ${CHCORE_CROSS_COMPILE}nm)
set(CMAKE_OBJCOPY ${CHCORE_CROSS_COMPILE}objcopy)
set(CMAKE_OBJDUMP ${CHCORE_CROSS_COMPILE}objdump)
set(CMAKE_RANLIB ${CHCORE_CROSS_COMPILE}ranlib)
set(CMAKE_STRIP ${CHCORE_MUSL_LIBC_INSTALL_DIR}/bin/musl-strip)

# Set build type
if(CHCORE_USER_DEBUG)
    set(CMAKE_BUILD_TYPE Debug)
else()
    set(CMAKE_BUILD_TYPE Release)
endif()

# Build position independent code, a.k.a -fPIC
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(${CMAKE_CURRENT_LIST_DIR}/_common.cmake)

# Set the target system (automatically set CMAKE_CROSSCOMPILING to true)
set(CMAKE_SYSTEM_NAME ChCore)
set(CMAKE_SYSTEM_PROCESSOR ${CHCORE_ARCH})

# Set prefix path
if(CHCORE_CHPM_INSTALL_PREFIX)
    # Get absolute path
    get_filename_component(_chpm_install_prefix ${CHCORE_CHPM_INSTALL_PREFIX}
                           REALPATH BASE_DIR ${CHCORE_PROJECT_DIR})

    # For find_package, find_library, etc.
    set(CMAKE_PREFIX_PATH ${_chpm_install_prefix})

    # C++ headers (FIXME: now we hardcode the version number)
    if(CHCORE_ARCH STREQUAL x86_64)
        include_directories(
            $<$<COMPILE_LANGUAGE:CXX>:${_chpm_install_prefix}/include/c++/9.2.0/x86_64-linux-musl>
        )
    elseif(CHCORE_ARCH STREQUAL aarch64)
        include_directories(
            $<$<COMPILE_LANGUAGE:CXX>:${_chpm_install_prefix}/include/c++/9.2.0/aarch64-linux-musleabi>
        )
    elseif(CHCORE_ARCH STREQUAL riscv64)
        include_directories(
            $<$<COMPILE_LANGUAGE:CXX>:${_chpm_install_prefix}/include/c++/9.2.0/riscv64-linux-musl>
    )
    else()
        message(
            WARNING
                "Please set arch-specific C++ header location for ${CHCORE_ARCH}"
        )
    endif()
    include_directories(
        $<$<COMPILE_LANGUAGE:CXX>:${_chpm_install_prefix}/include/c++/9.2.0>)

    # Link C++ standard library for C++ apps
    if(EXISTS ${CMAKE_PREFIX_PATH}/lib/libstdc++.so)
        set(CMAKE_CXX_FLAGS
        ${CMAKE_CXX_FLAGS} -L${_chpm_install_prefix}/lib/ ${_chpm_install_prefix}/lib/libstdc++.so ${_chpm_install_prefix}/lib/libgcc_s.so
        )
    endif()
endif()
