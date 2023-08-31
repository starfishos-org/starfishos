target_sources(
    ${kernel_target}
    PRIVATE cap_group.c
            capability.c
            irq.c
            memory.c
            recycle.c
            set_thread_env.c
            thread.c
            user_fault.c)
