#include <chcore/syscall.h>
#include <chcore/ipc.h>
#include <stdio.h>

#define assert(x)                                                 \
        do {                                                      \
                if (!(x)) {                                       \
                        printf("assertion failed: %s, line %d\n", \
                               __FILE__,                          \
                               __LINE__);                         \
                }                                                 \
        } while (0)

/* Implement kvstore using in-memory kvs */

#include "llkvs.h"

struct kvs *g_kvs = NULL;

void setup() {
    g_kvs = new_kvs(1000);
}

void reset() {
    if (g_kvs) {
        kvs_destroy(g_kvs);
    }
    setup();
}

/* Handle requests from clients */

enum kvs_cmd {GET, PUT, DEL, RESET};
enum response_code { OK = 0, NO_KEY = 1, ERROR = 2 };

struct kvs_req {
    enum kvs_cmd cmd;
    long long key;
    long long val_len;
    char value[0];
};

void kvs_server_dispatch(ipc_msg_t *ipc_msg, u64 client_badge)
{
    struct kvs_req *req = (struct kvs_req *)ipc_get_msg_data(ipc_msg);
    int code;
    switch (req->cmd) {
        case GET: {
            span_t *value = kvs_get(g_kvs, &req->key);
            if (value) {
                req->val_len = value->len;
                memcpy(req->value, value->data, value->len);
                code = OK;
            }
            else {
                code = NO_KEY;
            }
            break;
        }
        case PUT: {
            span_t span;
            span.len = req->val_len;
            if (span.len < 0) {
                code = ERROR;
                break;
            }
            span.data = malloc(req->val_len);
            memcpy(span.data, req->value, req->val_len);
            int ret = kvs_put(g_kvs, &req->key, &span);
            if (ret) {
                code = ERROR;
            }
            else {
                code = OK;
            }
            break;
        }
        case DEL: {
            kvs_del(g_kvs, &req->key);
            code = OK;
            break;
        }
        case RESET: {
            reset();
            code = OK;
            break;
        }
        default: {
            code = ERROR;
            break;
        }
    }
    ipc_return(ipc_msg, code);
}

int main() {
    /* Initialize kvstore */
    setup();

    /* Register IPC server */
    assert(!ipc_register_server(kvs_server_dispatch, DEFAULT_CLIENT_REGISTER_HANDLER));
	
    for (;;) {
		usys_yield();
	}

	return 0;
}
