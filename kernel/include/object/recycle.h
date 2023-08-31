#pragma once

#include <common/types.h>

/* Syscalls */
int sys_register_recycle(int notifc_cap, vaddr_t msg_buffer);
void sys_exit_group(int exitcode);
int sys_cap_group_recycle(int cap_group_cap);
