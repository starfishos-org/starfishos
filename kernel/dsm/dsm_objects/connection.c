#include <ipc/connection.h>

#include "../dsm_tiering.h"

int dsm_copy_connection(struct object *src_obj, struct object *dst_obj)
{
    struct ipc_connection *src_conn = (struct ipc_connection *)src_obj->opaque;
    struct ipc_connection *dst_conn = (struct ipc_connection *)dst_obj->opaque;

    // DSM_TIER_LOG_DEBUG("copy connection: %p, client thread: %p (%s), server thread: %p (%s)\n", 
    //     src_conn, src_conn->current_client_thread, 
    //     src_conn->current_client_thread ? src_conn->current_client_thread->cap_group->cap_group_name : "NULL", 
    //     src_conn->server_handler_thread, 
    //     src_conn->server_handler_thread ? src_conn->server_handler_thread->cap_group->cap_group_name : "NULL");

    /* Copy basic fields */
    dst_conn->client_badge = src_conn->client_badge;
    dst_conn->user_ipc_msg = src_conn->user_ipc_msg;
    dst_conn->shm = src_conn->shm;
    dst_conn->ownership = src_conn->ownership;
    dst_conn->conn_cap_in_client = src_conn->conn_cap_in_client;
    dst_conn->conn_cap_in_server = src_conn->conn_cap_in_server;
    dst_conn->state = src_conn->state;

    /* Threads will be handled separately */
    dst_conn->current_client_thread = src_conn->current_client_thread;
    dst_conn->server_handler_thread = src_conn->server_handler_thread;
    // dsm_demote_object(obj2object(src_conn->current_client_thread));
    // dsm_demote_object(obj2object(src_conn->server_handler_thread));

    // kinfo("copy connection finished%p\n", dst_conn);

    return 0;
}
