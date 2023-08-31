#pragma once

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VMR_READ    (1 << 0)
#define VMR_WRITE   (1 << 1)
#define VMR_EXEC    (1 << 2)
#define VMR_DEVICE  (1 << 3)
#define VMR_NOCACHE (1 << 4)

#define SHM_RDONLY 010000
#define SHM_RND    020000
#define SHM_REMAP  040000
#define SHM_EXEC   0100000

enum SHM_REQ {
	SHM_REQ_GET,
	SHM_REQ_CTL, // We don't support this op now.
};

struct shm_request {
	enum SHM_REQ req;
	int shmid;
	int key;
	size_t size;
	int flag;
};

#ifdef __cplusplus
}
#endif
