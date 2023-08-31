#pragma once

#include <chcore-internal/fs_defs.h>
#include <chcore/container/list.h>

/* ------------------------------------------------------------------------ */

/*
 * FSM will record all the caps of fs those are sended to some client.
 * Such information is recorded in the following structure.
 */
struct fsm_client_cap_node {
	u64 client_badge;

	/* TODO: using list instead */
	int cap_table[16];
	int cap_num;

	struct list_head node;
};

extern struct list_head fsm_client_cap_table;
extern pthread_mutex_t fsm_client_cap_table_lock;

int fsm_set_client_cap(u64 client_badge, int cap);
int fsm_get_client_cap(u64 client_badge, int cap);
