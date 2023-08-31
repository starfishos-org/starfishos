#pragma once
#include <chcore/type.h>

enum SERIAL_REQ {
	WRITE,
	READ,
	READ_NB,
};

enum SERIAL_TYPE {
	PL011,
};

struct write_read_struct {
	int size;
	char data[2048];
};

struct serial_request {
	int serial_req;
	int serial_type;
	union {
		struct write_read_struct rw_struct;
	};
};
