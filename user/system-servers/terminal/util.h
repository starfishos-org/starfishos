#pragma once

#include <stdio.h>
#include <stdlib.h>

#define check_ret(ret)                              \
	do {                                        \
		if ((ret) < 0) {                    \
			fprintf(stderr,             \
				"%s failed: %s:%d", \
				__func__,           \
				__FILE__,           \
				__LINE__);          \
			exit(-1);                   \
		}                                   \
	} while (0)

#define check_ptr(ptr)                              \
	do {                                        \
		if ((ptr) == NULL) {                \
			fprintf(stderr,             \
				"%s failed: %s:%d", \
				__func__,           \
				__FILE__,           \
				__LINE__);          \
			exit(-1);                   \
		}                                   \
	} while (0)
