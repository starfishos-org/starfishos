cmake_minimum_required(VERSION 3.14)
project(ChCoreKernel C ASM)

include(CommonTools)
include(KernelTools)

chcore_dump_cmake_vars()

# Create kernel.img target
set(kernel_target "kernel.img")
add_executable(${kernel_target})
install(TARGETS ${kernel_target} DESTINATION ${CMAKE_INSTALL_PREFIX})

# Set warning level
list(APPEND _compile_options -Wall)
list(APPEND _compile_options -Werror)

# Generic compile settings
list(APPEND _compile_definitions)
list(APPEND _compile_options -nostdinc -ffreestanding -fPIC -fno-omit-frame-pointer)
list(APPEND _asm_compile_definitions __ASM__)
list(APPEND _asm_compile_options)
list(APPEND _c_compile_definitions)
list(APPEND _c_compile_options)

# Arch-specific compile settings
if(CHCORE_ARCH STREQUAL "x86_64")
  list(APPEND _compile_options -mno-sse -mfsgsbase)
endif()

# This is only for `fbinfer` stage in CI script
if(FBINFER)
  list(APPEND _compile_definitions FBINFER)
endif()

# Set compile settings to target
target_compile_definitions(${kernel_target} PRIVATE ${_compile_definitions})
target_compile_options(${kernel_target} PRIVATE ${_compile_options})
target_compile_definitions(
    ${kernel_target}
    PRIVATE $<$<COMPILE_LANGUAGE:ASM>:${_asm_compile_definitions}>)
target_compile_options(
    ${kernel_target} PRIVATE $<$<COMPILE_LANGUAGE:ASM>:${_asm_compile_options}>)
target_compile_definitions(
    ${kernel_target} PRIVATE $<$<COMPILE_LANGUAGE:C>:${_c_compile_definitions}>)
target_compile_options(${kernel_target}
                       PRIVATE $<$<COMPILE_LANGUAGE:C>:${_c_compile_options}>)

# Linker options
target_link_options(${kernel_target} PRIVATE -no-pie -nostdlib -nostartfiles
                    -Wl,--build-id=none)

# Build-config-specific options
# FIXME(qyc): defining with hardcoded values is ugly
target_compile_definitions(${kernel_target}
                           PRIVATE $<$<CONFIG:Debug>:LOG_LEVEL=1>)
target_compile_definitions(${kernel_target}
                           PRIVATE $<$<CONFIG:Release>:LOG_LEVEL=1>)

# Include directories
target_include_directories(${kernel_target} PRIVATE include)
target_include_directories(${kernel_target} PRIVATE include/arch/${CHCORE_ARCH})
target_include_directories(
    ${kernel_target} PRIVATE include/arch/${CHCORE_ARCH}/plat/${CHCORE_PLAT})

# Add source code directories
add_subdirectory(arch/${CHCORE_ARCH})
add_subdirectory(arch/${CHCORE_ARCH}/plat/${CHCORE_PLAT})
#if(DSM_ENABLED)
add_subdirectory(dsm)
#endif()
if(CHCORE_SLS)
  add_subdirectory(ckpt)
endif()
if(CHCORE_SSI_SLS)
  add_subdirectory(ckpt-ssi)
endif()
add_subdirectory(ipc)
add_subdirectory(irq)
add_subdirectory(lib)
add_subdirectory(mm)
add_subdirectory(object)
add_subdirectory(sched)
add_subdirectory(syscall)
add_subdirectory(drivers)

# Kernel testing
if(CHCORE_KERNEL_TEST)
  add_subdirectory(tests)
endif()

macro(_kernel_incbin _binary_name _binary_path)
  set(binary_name ${_binary_name})
  set(binary_path ${_binary_path})
  configure_file(incbin.tpl.S incbin_${_binary_name}.S)
  unset(binary_name)
  unset(binary_path)
  target_sources(${kernel_target} PRIVATE incbin_${_binary_name}.S)
endmacro()

# enabel DRAM allocator
target_compile_definitions(${kernel_target} PRIVATE USE_DRAM)

if(CHCORE_SSI)
  include(dsm_config.cmake)
  # enable DSM configuration
  target_compile_definitions(${kernel_target} PRIVATE DSM_ENABLED)
  # target_compile_definitions(${kernel_target} PRIVATE MULTI_PAGETABLE_ENABLED)
  target_compile_definitions(${kernel_target} PRIVATE USE_CXL_MEM)
  # target_compile_definitions(${kernel_target} PRIVATE DSM_CLEAR_FIRST)
  target_compile_definitions(${kernel_target} PRIVATE DSM_LINEAR_MM_LAYOUT)
  # use which device to simulate the shm
  target_compile_definitions(${kernel_target} PRIVATE DSM_SHM_DEVICE_${DSM_SHM_DEVICE})
  # DSM memory configurations
  foreach(type MALLOC USER_MALLOC PGTABLE STACK THREADCTX OBJECT PAGE)
    target_compile_definitions(${kernel_target} PRIVATE DSM_${type}_MODE_${DSM_${type}_MODE})
  endforeach()
endif()

# SLS Basic Configruations
if(CHCORE_SLS)
  include(sls_config.cmake)
  # enable restore from latest checkpoint
  if("${SLS_RESTORE}" STREQUAL "ON")
    target_compile_definitions(${kernel_target} PRIVATE RESTORE_ENABLED)
  endif()
  # enable external sync
  if("${SLS_EXT_SYNC}" STREQUAL "ON")
    target_compile_definitions(${kernel_target} PRIVATE EXT_SYNC_ENABLED)
  endif()
  # enable hybrid memory copy methods; else will use default cow
  if("${SLS_HYBRID_MEM}" STREQUAL "ON")
    target_compile_definitions(${kernel_target} PRIVATE TRACK_REVERSE_ENABLED)
    target_compile_definitions(${kernel_target} PRIVATE HYBRID_MEM)
    target_compile_definitions(${kernel_target} PRIVATE PARALLEL_LOOP)
  endif()

  # memory allocator
  target_compile_definitions(${kernel_target} PRIVATE USE_NVM)

  # SLS Report Details
  if("${SLS_REPORT_CKPT}" STREQUAL "ON")
    target_compile_definitions(${kernel_target} PRIVATE REPORT)
  endif()
  if("${SLS_REPORT_RESTORE}" STREQUAL "ON")
    target_compile_definitions(${kernel_target} PRIVATE RESTORE_REPORT)
  endif()
  if("${SLS_REPORT_HYBRID}" STREQUAL "ON")
    target_compile_definitions(${kernel_target} PRIVATE REPORT_RUNTIME)
    target_compile_definitions(${kernel_target} PRIVATE REPORT_HYBRID_MEM)
    # target_compile_definitions(${kernel_target} PRIVATE REPORT_HYBRID_MEM_DEBUG)
  endif()

  # for tests of only-checkpoint, +pf, +memcpy
  if("${SLS_SPECIAL_OMIT_PF}" STREQUAL "ON")
    target_compile_definitions(${kernel_target} PRIVATE OMIT_PF)
  endif()

  if("${SLS_SPECIAL_OMIT_MEMCPY}" STREQUAL "ON")
    target_compile_definitions(${kernel_target} PRIVATE OMIT_MEMCPY)
  endif()

  if("${SLS_SPECIAL_OMIT_BENCHMARK}" STREQUAL "ON")
    target_compile_definitions(${kernel_target} PRIVATE OMIT_BENCHMARK)
  endif()
endif()

if(CHCORE_IPC_PERF_ENABLED)
  target_compile_definitions(${kernel_target} PRIVATE IPC_PERF_ENABLED)
endif()

# correctness check
# target_compile_definitions(${kernel_target} PRIVATE PMO_CHECKSUM)

# Include essential user-level binaries
_kernel_incbin(procmgr_bin ${CHCORE_USER_INSTALL_DIR}/system-servers/procmgr/procmgr)
