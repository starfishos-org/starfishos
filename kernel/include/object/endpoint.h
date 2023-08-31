#pragma once

#include <object/object.h>
#include <object/thread.h>

enum endpoint_invlabel {
	ENDPOINT_INVLABEL_RECV = 0,
	ENDPOINT_INVLABEL_SEND,
};

enum endpoint_state {
	ENDPOINT_STATE_IDLE = 0,
	ENDPOINT_STATE_SEND,
	ENDPOINT_STATE_RECV,
};

struct endpoint {
	int state;
	// TODO: should be a list
	struct thread *queue;
};

int endpoint_invoke(struct endpoint *endpoint_invoke, int invlabel,
		    u64 *args, u64 rights);
