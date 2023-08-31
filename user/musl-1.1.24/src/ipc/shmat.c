#include <sys/shm.h>
#include <bits/errno.h>
#include <chcore-internal/shmmgr_defs.h>
#include <chcore/bug.h>
#include <chcore/syscall.h>
#include <stdio.h>

/*
 * Find free space and map pmo there
*/
void *shmat(int id, const void *addr, int flag)
{
#if SYSTEMV_SHMMGR == ON
	u64 shmaddr = 0;
	u64 perm = 0;

	if ((shmaddr != 0) && (flag & SHM_RND)) {
		printf("No support for specific shmaddr now.\n");
		shmaddr = -EINVAL;
		goto out;
	}

	if (flag & SHM_REMAP) {
		printf("No support for SHM_REMAP now.\n");
		shmaddr = -EINVAL;
		goto out;
	}

	/* Set the permission */
	if (flag & SHM_RDONLY) {
		perm = VMR_READ;
	} else if (flag & SHM_EXEC) {
		perm = VMR_READ | VMR_WRITE | VMR_EXEC;
	} else {
		perm = VMR_READ | VMR_WRITE;
	}

	shmaddr = usys_map_with_pmo(id, perm);

out:
	return (void*)shmaddr;
#else
	return (void*)0;
#endif
}
