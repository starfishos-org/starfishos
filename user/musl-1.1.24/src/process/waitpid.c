#include <sys/wait.h>
#include "syscall.h"

#include <assert.h>
#include <chcore/proc.h>

pid_t waitpid(pid_t pid, int *status, int options)
{
	assert(0);
	return chcore_waitpid(pid, status, options, 0);
	return syscall_cp(SYS_wait4, pid, status, options, 0);
}
