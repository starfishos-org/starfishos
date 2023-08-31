#include <malloc.h>
#include <string.h>
#include "fsm_client_cap.h"

struct list_head fsm_client_cap_table;

/* Return mount_id */
int fsm_set_client_cap(u64 client_badge, int cap)
{
	struct fsm_client_cap_node *n;

	for_each_in_list(n, struct fsm_client_cap_node, node, &fsm_client_cap_table) {
		if (n->client_badge == client_badge) {
			/* Client already visited */
			BUG_ON(n->cap_num >= 16);
			n->cap_table[n->cap_num] = cap;
			n->cap_num++;
			/* TODO: mount_id */
			return n->cap_num - 1;
		}
	}

	/* Client is not visited, create a new fsm_client_cap_node */
	n = (struct fsm_client_cap_node *)malloc(sizeof(*n));
	n->client_badge = client_badge;
	memset(n->cap_table, 0, sizeof(n->cap_table));
	n->cap_table[0] = cap;
	n->cap_num = 1;

	list_append(&n->node, &fsm_client_cap_table);

	/* TODO: mount_id */
	return 0;
}

/* Return mount_id if record exists, otherwise -1 */
int fsm_get_client_cap(u64 client_badge, int cap)
{
	struct fsm_client_cap_node *n;
	int i;

	for_each_in_list(n, struct fsm_client_cap_node, node, &fsm_client_cap_table)
		if (n->client_badge == client_badge)
			for (i = 0; i < n->cap_num; i++)
				if (n->cap_table[i] == cap)
					return i;       // TODO: mount_id

	return -1;
}
