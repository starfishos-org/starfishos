#include <sys/shm.h>
#include <endian.h>
#include <stdio.h>
#include <chcore-internal/shmmgr_defs.h>

int shmctl(int id, int cmd, struct shmid_ds *buf)
{
	printf("No support for shmctl now.\n");
	return 0;
}
