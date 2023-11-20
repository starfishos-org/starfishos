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

if(RMAP_ENABLED)
  target_sources(
    ${kernel_target}
    PRIVATE rmap.c
  )
endif()
