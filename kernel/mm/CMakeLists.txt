target_sources(
    ${kernel_target}
    PRIVATE buddy.c
            kmalloc.c
            mm.c
            pgfault_handler.c
            slab.c
            vmregion.c
            dram_alloc.c
            dram_slab.c)

if(CHCORE_SLS)
  target_sources(
    ${kernel_target}
    PRIVATE rmap.c
  )
endif()

if(CHCORE_SSI)
target_sources(
  ${kernel_target}
  PRIVATE cxl_alloc.c
          cxl_slab.c
          temp_alloc.c
          temp_slab.c
)
endif()
