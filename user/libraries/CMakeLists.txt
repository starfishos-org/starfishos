set(_libraries_clean_targets)

add_subdirectory(sys-libs)
if(CHCORE_LIBRARY_DEVICE)
    add_subdirectory(dev_sdk)
endif()
if(CHCORE_LIBRARY_GRAPHIC)
    add_subdirectory(graphic)
endif()
if(CHCORE_LIBRARY_LKL)
    add_subdirectory(lkl-libs)
endif()

if(CHCORE_DEMOS_CPYTHON)
if (NOT EXISTS ${libc_install_dir}/lib/libffi.so)
  add_custom_target(
    libffi
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libffi
    COMMAND ./configure CXX=${MUSL_CXX} CC=${MUSL_CC} --disable-docs --host=x86_64-linux-musl --prefix=${libc_install_dir}
    COMMAND make -j$(_nproc)
    COMMAND make install
    DEPENDS
  )
else()
  add_custom_target(
    libffi
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libffi
    DEPENDS
  )
endif()

add_custom_target(
    libffi-clean
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libffi
    COMMAND make clean
)
add_dependencies(ramdisk libffi)

if (NOT EXISTS ${libc_install_dir}/lib/libz.so)
  add_custom_target(
    zlib
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/zlib
    COMMAND ./configure --prefix=${libc_install_dir}
    COMMAND make -j$(_nproc) CC=${MUSL_CC} CXX=${MUSL_CXX}
    COMMAND make install
    DEPENDS
  )
else()
add_custom_target(
    zlib
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/zlib
    DEPENDS
  )
endif()
add_custom_target(
    zlib-clean
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/zlib
    COMMAND make clean
)
add_dependencies(ramdisk zlib)
endif()
add_custom_target(libraries-clean DEPENDS ${_libraries_clean_targets})
