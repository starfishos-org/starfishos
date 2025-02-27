#include <common/debug.h>
#include <common/kprint.h>

#if BACKTRACE_FUNC == ON

int backtrace(void)
{
    kinfo("backtrace on x86_64 is not implemented.\n");
    return 0;
}

#endif
