#include <ipc/connection.h>

#include "../dsm_tiering.h"

extern int dsm_copy_ipc_config(void *src_ipc_config, void *dst_ipc_config, mem_t mem_type);
extern int dsm_copy_shadow_thread(struct thread *src_thread, struct thread *dst_thread, mem_t mem_type);
void dsm_copy_shm_for_ipc_connection(struct shm_for_ipc_connection *src_shm, struct shm_for_ipc_connection *dst_shm)
{
    dst_shm->client_shm_uaddr = src_shm->client_shm_uaddr;
    dst_shm->server_shm_uaddr = src_shm->server_shm_uaddr;
    dst_shm->shm_size = src_shm->shm_size;
    dst_shm->shm_cap_in_client = src_shm->shm_cap_in_client;
    dst_shm->shm_cap_in_server = src_shm->shm_cap_in_server;
    DSM_TIER_LOG_DEBUG("copy shm for ipc connection: %p, shm_size: %d, shm_cap_in_client: %d, shm_cap_in_server: %d\n", 
        dst_shm, dst_shm->shm_size, dst_shm->shm_cap_in_client, dst_shm->shm_cap_in_server);
}

int dsm_copy_connection(struct object *src_obj, struct object *dst_obj)
{
    struct ipc_connection *src_conn = (struct ipc_connection *)src_obj->opaque;
    struct ipc_connection *dst_conn = (struct ipc_connection *)dst_obj->opaque;
    mem_t mem_type = is_private_object(src_obj) ? __MT_SHARED__ : __MT_PRIVATE__;
    int ret = 0;

    DSM_TIER_LOG_DEBUG("copy connection: %p, client thread: %p (%s), server thread: %p (%s)\n", 
        src_conn, src_conn->current_client_thread, 
        src_conn->current_client_thread ? src_conn->current_client_thread->cap_group->cap_group_name : "NULL", 
        src_conn->server_handler_thread, 
        src_conn->server_handler_thread ? src_conn->server_handler_thread->cap_group->cap_group_name : "NULL");

    /* Copy basic fields */
    dst_conn->client_badge = src_conn->client_badge;
    dst_conn->ownership = src_conn->ownership;
    dst_conn->conn_cap_in_client = src_conn->conn_cap_in_client;
    dst_conn->conn_cap_in_server = src_conn->conn_cap_in_server;
    dst_conn->state = src_conn->state;

    /* Copy ipc msg and shm */
    if (!dst_conn->user_ipc_msg) {
        dst_conn->user_ipc_msg = (struct ipc_msg *)kmalloc(sizeof(struct ipc_msg), mem_type);
        BUG_ON(!dst_conn->user_ipc_msg);
    }
    DSM_TIER_LOG_DEBUG("user ipc msg: %p\n", dst_conn->user_ipc_msg);
    src_conn->user_ipc_msg = dst_conn->user_ipc_msg;

    dsm_copy_shm_for_ipc_connection(&src_conn->shm, &dst_conn->shm);

    /* Threads will be handled separately */
    dst_conn->current_client_thread = src_conn->current_client_thread;
    dst_conn->server_handler_thread = src_conn->server_handler_thread;

    /* Create a hooked object for the connection*/
    struct object *obj, *shared_obj;

    obj = obj2object(src_conn->server_handler_thread);
    shared_obj = dsm_get_object_by_mem_type(obj, mem_type, true);
    shared_obj->status = DSM_STATUS_INVALID;
    shared_obj->dsm_type = DSM_TYPE_CROSS_SHARED;
    dst_conn->server_handler_thread = (struct thread *)object2obj(shared_obj);
    ret = dsm_copy_shadow_thread(src_conn->server_handler_thread, dst_conn->server_handler_thread, mem_type);
    if (ret != 0) {
        DSM_TIER_LOG_ERR("copy shadow thread failed\n");
        return ret;
    }

    // print_thread(src_conn->server_handler_thread);

    obj = obj2object(src_conn->current_client_thread);
    shared_obj = dsm_get_object_by_mem_type(obj, mem_type, true);
    dst_conn->current_client_thread = (struct thread *)object2obj(shared_obj);

    // print_thread(src_conn->current_client_thread);

    return 0;
}
