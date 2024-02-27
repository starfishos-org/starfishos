target_sources(${kernel_target} PRIVATE 
    memop.c
    mmparse.c
    page_table.c 
    tlb.c
    numa.c
    uaccess.c 
    vmspace.c)
