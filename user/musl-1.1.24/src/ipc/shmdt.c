#include <sys/shm.h>
#include <chcore/type.h>
#include <chcore/syscall.h>
#include <chcore-internal/shmmgr_defs.h>

/*
 * Unmap shm pmo at addr
*/
int shmdt(const void *addr)
{
#if SYSTEMV_SHMMGR == ON
	int ret;

	ret = usys_unmap_with_addr((u64)addr);

	return ret;
#else
	return 0;
#endif
}
