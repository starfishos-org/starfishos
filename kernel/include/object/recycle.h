#pragma once

struct cap_group;

#include <common/types.h>

/* Syscalls */
int sys_register_recycle(int notifc_cap, vaddr_t msg_buffer);
void sys_exit_group(int exitcode);
int sys_cap_group_recycle(int cap_group_cap);
#ifdef DSM_ENABLED
void cap_group_request_cross_machine_exit(struct cap_group *cap_group,
                                          mid_t failed_machine_id,
                                          int exitcode);
#endif
