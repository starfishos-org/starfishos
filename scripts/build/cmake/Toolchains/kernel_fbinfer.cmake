set(CHCORE_CROSS_COMPILE
    "aarch64-linux-gnu-"
    CACHE STRING "" FORCE)
set(CHCORE_PLAT
    "raspi3"
    CACHE STRING "" FORCE)

set(FBINFER ON)

include(${CMAKE_CURRENT_LIST_DIR}/kernel.cmake)
