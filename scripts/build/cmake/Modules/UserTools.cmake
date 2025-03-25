function(chcore_install_target_to_ramdisk _target)
    install(TARGETS ${_target} DESTINATION ${CMAKE_INSTALL_PREFIX}/ramdisk)
    set_property(GLOBAL PROPERTY ${_target}_INSTALLED TRUE)
endfunction()

function(chcore_install_binary_to_ramdisk _file)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${_file}
            DESTINATION ${CMAKE_INSTALL_PREFIX}/ramdisk)
endfunction()

# Get all "build system targets" defined in the current source dir,
# recursively.
function(chcore_get_all_targets _out_var)
    set(_targets)
    _get_all_targets_recursive(_targets ${CMAKE_CURRENT_SOURCE_DIR})
    set(${_out_var}
        ${_targets}
        PARENT_SCOPE)
endfunction()

macro(_get_all_targets_recursive _targets _dir)
    get_property(
        _subdirectories
        DIRECTORY ${_dir}
        PROPERTY SUBDIRECTORIES)
    foreach(_subdir ${_subdirectories})
        _get_all_targets_recursive(${_targets} ${_subdir})
    endforeach()

    get_property(
        _current_targets
        DIRECTORY ${_dir}
        PROPERTY BUILDSYSTEM_TARGETS)
    list(APPEND ${_targets} ${_current_targets})
endmacro()

function(chcore_copy_binary_to_ramdisk _target)
    add_custom_target(
        cp_${_target}_to_ramdisk
        COMMAND rsync -a ${CMAKE_CURRENT_BINARY_DIR}/${_target} ${build_ramdisk_dir}
        DEPENDS ${_target})
    add_dependencies(ramdisk cp_${_target}_to_ramdisk ${_target})
    set_property(GLOBAL PROPERTY ${_target}_INSTALLED TRUE)
endfunction()

function(chcore_copy_target_to_ramdisk _target)
    add_custom_target(
        cp_${_target}_to_ramdisk
        COMMAND rsync -a $<TARGET_FILE:${_target}> ${build_ramdisk_dir}
        DEPENDS ${_target})
    add_dependencies(ramdisk cp_${_target}_to_ramdisk ${_target})
    set_property(GLOBAL PROPERTY ${_target}_INSTALLED TRUE)
endfunction()

# Install all shared library and executable targets defined in
# the current source dir to ramdisk.
#
# This will exclude those that are already installed by
# `chcore_install_target_as_cpio` or `chcore_install_target_to_ramdisk`.
function(chcore_copy_all_targets_to_ramdisk)
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
            chcore_copy_target_to_ramdisk(${_target})
        endif()
    endforeach()
endfunction()

function(chcore_enable_clang_tidy)
    set(_checks
        "-bugprone-easily-swappable-parameters,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-bugprone-reserved-identifier"
    )
    set(_options)
    set(_one_val_args EXTRA_CHECKS)
    set(_multi_val_args)
    cmake_parse_arguments(_clang_tidy "${_options}" "${_one_val_args}"
                          "${_multi_val_args}" ${ARGN})
    if(_clang_tidy_EXTRA_CHECKS)
        set(_checks "${_checks},${_clang_tidy_EXTRA_CHECKS}")
    endif()
    set(CMAKE_C_CLANG_TIDY
        clang-tidy --checks=${_checks}
        --extra-arg=-I${CHCORE_MUSL_LIBC_INSTALL_DIR}/include
        --config-file=${CHCORE_PROJECT_DIR}/.clang-tidy
        PARENT_SCOPE)
endfunction()

function(chcore_disable_clang_tidy)
    unset(CMAKE_C_CLANG_TIDY PARENT_SCOPE)
endfunction()

function(chcore_copy_files_to_ramdisk)
    file(COPY ${ARGN} DESTINATION ${build_ramdisk_dir})
endfunction()

function(chcore_objcopy_binary _user_target _binary_name)
    add_custom_target(
        ${_binary_name} ALL
        COMMAND ${CMAKE_OBJCOPY} -O binary -S $<TARGET_FILE:${_user_target}>
                ${_binary_name}
        DEPENDS ${_user_target})
endfunction()