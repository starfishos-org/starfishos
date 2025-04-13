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

/* reuse old ipc, call remote ipc */
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
    dst_conn->trans_machine = 1;

    /* Copy ipc msg and shm */
    if (likely(!dst_conn->user_ipc_msg)) {
        dst_conn->user_ipc_msg = (struct ipc_msg *)
            kmalloc(sizeof(struct ipc_msg), mem_type);
        BUG_ON(!dst_conn->user_ipc_msg);
    }
    dsm_copy_shm_for_ipc_connection(&src_conn->shm, &dst_conn->shm);

    /* Step-1: copy server handler thread, which is a SHADOW thread */
    /* Create a hooked object for the connection*/
    struct object *obj, *shared_obj;

    // demote server handler thread
    obj = obj2object(src_conn->server_handler_thread);
    ret = dsm_demote_object(obj);
    if (ret != 0) {
        DSM_TIER_LOG_ERR("demote server handler thread failed\n");
        return ret;
    }
    shared_obj = dsm_get_object_by_mem_type(obj, mem_type, false);
    BUG_ON(!shared_obj);
    dst_conn->server_handler_thread = (struct thread *)object2obj(shared_obj);

    // print_thread(src_conn->server_handler_thread);

    /* Step-2: copy the client thread, which is the thread itself */
    obj = obj2object(src_conn->current_client_thread);
    shared_obj = dsm_get_object_by_mem_type(obj, mem_type, true);
    dst_conn->current_client_thread = (struct thread *)object2obj(shared_obj);

    // print_thread(src_conn->current_client_thread);

    return 0;
}

int dsm_ckpt_connection(struct object *src_obj, struct object *dst_obj)
{
    return 0;
}
