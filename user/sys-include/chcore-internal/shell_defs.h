#pragma once

#define SHELL_REQ_BUFSIZE 1024

enum SHELL_REQ {
	SHELL_SET_PROCESS_INFO = 1,
	SHELL_APPEND_INPUT_BUFFER,
};

struct shell_req {
	int req;
	pid_t pid;
	char buf[SHELL_REQ_BUFSIZE];
	size_t size;
};
