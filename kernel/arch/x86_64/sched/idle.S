#include <common/asm.h>
#include <common/vars.h>

BEGIN_FUNC(idle_thread_routine)
idle:   hlt
        jmp idle
END_FUNC(idle_thread_routine)
