#include <unistd.h>
#include "syscall.h"

ssize_t pread(int fd, void *buf, size_t size, off_t ofs)
{
	return __syscall4(SYS_pread64, fd, buf, size, __SYSCALL_LL_PRW(ofs));
}

weak_alias(pread, pread64);
