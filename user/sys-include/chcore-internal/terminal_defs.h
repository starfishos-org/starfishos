#pragma once

#include <chcore/type.h>

#define TERMINAL_REQ_BUFSIZE 1024

enum TERMINAL_REQ {
	TERMINAL_REQ_PUT = 1,
};

struct terminal_request {
	enum TERMINAL_REQ req;
	char buffer[TERMINAL_REQ_BUFSIZE];
	size_t size;
};
