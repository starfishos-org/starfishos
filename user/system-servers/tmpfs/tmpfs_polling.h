#pragma once

#include <pthread.h>

/*
 * Polling thread for tmpfs: dequeues requests from a durable_queue on CXL SHM
 * and handles them directly inside tmpfs, bypassing IPC entirely.
 *
 * The polling thread uses a dedicated client_badge for fd→fid mapping,
 * so polling clients share a single fd namespace.
 */

/* Fixed badge used for all polling-queue clients */
#define POLLING_CLIENT_BADGE 0xA0A0ULL

/* Start the tmpfs polling thread. Returns 0 on success. */
int tmpfs_start_polling_thread(void);
