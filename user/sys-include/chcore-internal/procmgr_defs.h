#pragma once

#include <chcore/ipc.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

enum PROC_REQ {
	PROC_REQ_FORK = 1,
	PROC_REQ_CLONE,
	PROC_REQ_SPAWN,
	PROC_REQ_NEWPROC,
	PROC_REQ_WAIT,
	PROC_REQ_GET_MT_CAP,
	PROC_REQ_GET_SERVER_CAP,
	PROC_REQ_RECV_SIG,
	PROC_REQ_CHECK_STATE,
	PROC_REQ_GET_SHELL_CAP,
	PROC_REQ_SET_SHELL_CAP,
	PROC_REQ_GET_TERMINAL_CAP,
	PROC_REQ_SET_TERMINAL_CAP,
	PROC_CHILD_FINISH_FORK,
	PROC_REQ_MAX
};

enum CONFIGURABLE_SERVER {
	SERVER_TMPFS = 1,
	SERVER_SYSTEMV_SHMMGR,
	SERVER_HDMI_DRIVER,
	SERVER_SD_CARD,
	SERVER_FAT32_FS,
	SERVER_EXT4_FS,
	SERVER_USB_DEVMGR,
	SERVER_SERIAL,
	SERVER_GPIO,
	SERVER_GUI,
	CONFIG_SERVER_MAX,
};

#define PROC_REQ_NAME_LEN  255
#define PROC_REQ_TEXT_SIZE 256
#define PROC_REQ_ARGC_MAX  255

struct proc_request {
	enum PROC_REQ req;
	union {
		int argc; // Used by IPC client when creating new process
		pid_t pid; // Used by IPC client when wait on a process
		int exitstatus; // As the reply of process wait
		enum CONFIGURABLE_SERVER server_id; // Used to get server cap
		char sig; // Used by shell when shell receives a special character
	};
	// Used by procmgr to return the pcid of cloned process
	unsigned long pcid;
	// The following three are used by IPC client when creating new process.
	char name[PROC_REQ_NAME_LEN];
	off_t argv[PROC_REQ_ARGC_MAX];
	char text[PROC_REQ_TEXT_SIZE];
};

#ifdef __cplusplus
}
#endif
