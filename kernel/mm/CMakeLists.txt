file(GLOB src "*.c")

file(GLOB_RECURSE llfree_src "llfree/*.c")
list(APPEND src ${llfree_src})

if(CHCORE_SLS)
  file(GLOB_RECURSE sls_src "sls/*.c")
  list(APPEND src ${sls_src})
endif()

if(CHCORE_SSI)
  file(GLOB_RECURSE ssi_src "cxl/*.c" "temp/*c")
  list(APPEND src ${ssi_src})
endif()

target_sources(${kernel_target} PRIVATE ${src})
