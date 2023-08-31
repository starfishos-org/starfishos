#pragma once

#include <assert.h>
#include <chcore/cpio.h>
#include <chcore/defs.h>
#include <chcore/error.h>
#include <chcore-internal/fs_defs.h>
#include <chcore/container/list.h>
#include <chcore/ipc.h>
#include <chcore/launch_kern.h>
#include <chcore/launcher.h>
#include <chcore/proc.h>
#include <chcore/syscall.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>

#include "defs.h"

/* for debugging */
#include <chcore-internal/fs_debug.h>

int init_fsm(void);

int fsm_mount_fs(const char *path, const char *mount_point);
int fsm_umount_fs(const char *path);

void fsm_dispatch(ipc_msg_t *ipc_msg, u64 client_badge);
