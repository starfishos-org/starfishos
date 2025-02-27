#include "../ckpt_objects.h"
#include "../ckpt_object_pool.h"

#ifdef REPORT
extern u64 eval_obj_time[];
#endif

int connection_ckpt(struct ipc_connection *conn,
                    struct ckpt_ipc_connection *ckpt_conn, int alloc)
{
    struct object *old_client_obj, *old_server_obj;
    struct ckpt_obj_root *new_client_obj_root, *new_server_obj_root;
    int r;
    // BUG_ON(!(conn->server_handler_thread));
    if (conn->current_client_thread) {
        old_client_obj = container_of(
                conn->current_client_thread, struct object, opaque);
        new_client_obj_root = ckpt_obj_root_get(old_client_obj, alloc);
        if (!new_client_obj_root) {
            BUG_ON(1);
            r = -ENOMEM;
            goto out_fail;
        }
        ckpt_conn->current_client_thread_root = new_client_obj_root;
    } else {
        ckpt_conn->current_client_thread_root = NULL;
    }

    if (conn->server_handler_thread) {
        old_server_obj = container_of(
                conn->server_handler_thread, struct object, opaque);
        new_server_obj_root = ckpt_obj_root_get(old_server_obj, alloc);
        if (!new_server_obj_root) {
            BUG_ON(1);
            r = -ENOMEM;
            goto out_fail;
        }
        ckpt_conn->server_handler_thread_root = new_server_obj_root;
    } else {
        ckpt_conn->server_handler_thread_root = NULL;
    }

    ckpt_conn->client_badge = conn->client_badge;
    ckpt_conn->user_ipc_msg = conn->user_ipc_msg;
    ckpt_conn->shm = conn->shm;
    ckpt_conn->ownership = conn->ownership;
    ckpt_conn->conn_cap_in_client = conn->conn_cap_in_client;
    ckpt_conn->conn_cap_in_server = conn->conn_cap_in_server;
    ckpt_conn->is_valid = conn->is_valid;
    return 0;
out_fail:
    return r;
}

int connection_restore(struct object *conn_obj,
                       struct ckpt_object *ckpt_conn_obj, struct kvs *obj_map)
{
    int r;
    struct ipc_connection *conn = (struct ipc_connection *)conn_obj->opaque;
    struct ckpt_ipc_connection *ckpt_conn =
            (struct ckpt_ipc_connection *)ckpt_conn_obj->opaque;
    struct object *new_client_obj, *new_server_obj;

    if (ckpt_conn->current_client_thread_root) {
        new_client_obj = restore_obj_get(ckpt_conn->current_client_thread_root);
        if (!new_client_obj) {
            r = -ENOMEM;
            goto out_fail;
        }
        conn->current_client_thread = (struct thread *)new_client_obj->opaque;
    } else {
        conn->current_client_thread = NULL;
    }

    if (ckpt_conn->server_handler_thread_root) {
        new_server_obj = restore_obj_get(ckpt_conn->server_handler_thread_root);
        if (!new_server_obj) {
            r = -ENOMEM;
            goto out_fail;
        }

        conn->server_handler_thread = (struct thread *)new_server_obj->opaque;
    } else {
        conn->server_handler_thread = NULL;
    }

    conn->client_badge = ckpt_conn->client_badge;
    conn->user_ipc_msg = ckpt_conn->user_ipc_msg;
    conn->shm = ckpt_conn->shm;
    conn->ownership = ckpt_conn->ownership;
    conn->conn_cap_in_client = ckpt_conn->conn_cap_in_client;
    conn->conn_cap_in_server = ckpt_conn->conn_cap_in_server;
    conn->is_valid = ckpt_conn->is_valid;
    return 0;
out_fail:
    return r;
}
