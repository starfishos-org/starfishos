#pragma once
#include <chcore/type.h>
#include <chcore/container/list.h>

#define BUFLEN                 4096
#define MAX_HISTORY_CMD_RECORD 10

extern int fsm_scan_pmo_cap;
extern void *fsm_scan_buf;

/* fsm_server_cap in current process; can be copied to others */
extern int fsm_server_cap;
/* lwip_server_cap in current process; can be copied to others */
extern int lwip_server_cap;

extern bool waitchild;

struct history_cmd_node {
	char *cmd;
	int index;
	struct list_head cmd_node;
};
extern struct history_cmd_node *history_cmd_pointer;

void init_buildin_cmd(void);

void add_cmd_to_history(char *cmd);
void clear_history_point(void);

int do_complement(char *buf, char *complement, int complement_time);
int do_cd(char *cmdline);
int do_top(void);
int do_ls(char *cmdline);
void do_clear(void);
void do_jobs(void);
int do_fg(char *cmdline);
void do_history(void);
bool do_up(void);
bool do_down(void);
int do_source(char *cmdline);