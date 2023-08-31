#pragma once

#include <bits/errno.h>
#include <chcore/type.h>
#include <chcore/ipc.h>
#include <chcore/bug.h>
#include <chcore/syscall.h>
#include <pthread.h>
#include <malloc.h>
#include <assert.h>
#include <chcore-internal/fs_defs.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <string.h>

#include <fs_wrapper_defs.h>
#include <chcore-internal/fs_debug.h>
#include "ff.h"
#include "diskio.h"

void free_filfd(int fd);
int find_filfd(int isfile);
int flags_posix2internal(int flags);
void fill_stat(struct stat *statbuf, FILINFO *fno, ino_t inum);

