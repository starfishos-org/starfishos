#pragma once

#include <sys/types.h>
#include <string.h>
#include <chcore/pci_ioctl.h>

#include "fd.h"

#define IS_DEVFS(path) (strncmp(path, "/dev/", 5) == 0)
#define IS_DEV_VIRTIO_FS(path) (strncmp(path, "/dev/virtio", 11) == 0)

extern struct fd_ops virtio_file_ops;

int chcore_virtio_file_ioctl(int fd, unsigned long request, void *arg);
int chcore_open_dev(int fd, char *path);
