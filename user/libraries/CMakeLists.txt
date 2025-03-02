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

add_custom_target(libraries-clean DEPENDS ${_libraries_clean_targets})
