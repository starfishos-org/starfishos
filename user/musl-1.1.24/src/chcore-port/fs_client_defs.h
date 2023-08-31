#pragma once

#include <chcore-internal/fs_defs.h>
#include <chcore/container/list.h>
#include <assert.h>
#include <chcore/idman.h>
#include <pthread.h>

#include "fd.h"

/* This is statically linked to the `CLIENT process` */

#define MAX_CWD_LEN 255
#define MAX_CWD_BUF_LEN MAX_CWD_LEN + 1
#define MAX_PATH_LEN 511
#define MAX_PATH_BUF_LEN MAX_PATH_LEN + 1

/* +++++++++++++++++++++++++ Client Metadata ++++++++++++++++++++++++++++++ */

/* current work directory (cwd) */
extern char cwd_path[MAX_CWD_BUF_LEN];
extern int cwd_len;

/* ++++++++++++++++++++++++ Client IPC Pool +++++++++++++++++++++++++++++++ */

#define MAX_MOUNT_ID 32
extern int mounted_fs_cap[MAX_MOUNT_ID];
extern pthread_key_t mounted_fs_key;

ipc_struct_t *get_ipc_struct_by_mount_id(int mount_id);

/* ++++++++++++++++++++++++ File Descriptor Extension +++++++++++++++++++++ */

struct fd_record_extension {
	char path[MAX_PATH_LEN + 1];
	int mount_id;
};

/* Return new fd_record_extension struct */
struct fd_record_extension *_new_fd_record_extension();

/* ++++++++++++++++++++++++ Path Resolving && Utilities +++++++++++++++++++ */

char *path_from_fd(int fd);
int pathcpy(char *dst, size_t dst_buf_size, const char *path, size_t path_len);
char *path_join(const char *base, const char *next);
char *get_server_path(char *full_path, int full_len, char *mount_path, int mount_len);

/* ++++++++++++++++++++++++ IPC Library ++++++++++++++++++++++++++++++++ */

struct fsm_request *fsm_parse_path_forward(ipc_msg_t *ipc_msg, const char *full_path);

/* ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++ */

void init_fs_client_side();
