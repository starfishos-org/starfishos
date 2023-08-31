#pragma once

#include <chcore/type.h>

#ifdef __cplusplus
extern "C" {
#endif

/* TODO: consider where to put this header file. */

enum SD_REQ {
	SD_REQ_READ = 1,
	SD_REQ_WRITE,
	SD_REQ_MAX,
};

struct sd_msg {
	enum SD_REQ req;
	int block_number;
	int op_size;
	char pbuffer[6 * 512];
};

#ifdef __cplusplus
}
#endif
