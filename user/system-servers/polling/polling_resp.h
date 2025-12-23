#pragma once

#include "polling.h"

void handle_polling_fs_request(struct shm_msg *msg);

void handle_polling_fs_open(struct shm_msg *msg);
void handle_polling_fs_read(struct shm_msg *msg);
void handle_polling_fs_write(struct shm_msg *msg);
void handle_polling_fs_close(struct shm_msg *msg);
void handle_polling_fs_empty(struct shm_msg *msg);
