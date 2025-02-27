file(GLOB src "*.c")

if(CHCORE_SLS)
  file(GLOB_RECURSE sls_src "sls/*.c")
  list(APPEND src ${sls_src})
endif()

if(CHCORE_SSI)
  file(GLOB_RECURSE ssi_src "cxl/*.c" "temp/*c")
  list(APPEND src ${ssi_src})
endif()

target_sources(${kernel_target} PRIVATE ${src})
