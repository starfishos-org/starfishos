#include <common/kvstore.h>
#include <sched/sched.h>
#include <ipc/connection.h>

#include "../dsm_tiering.h"

static inline void dsm_copy_ipc_server_register_cb_config(void *src_ipc_config, void *dst_ipc_config)
{
    struct ipc_server_register_cb_config *src =
            (struct ipc_server_register_cb_config *)src_ipc_config;
    struct ipc_server_register_cb_config *dst =
            (struct ipc_server_register_cb_config *)dst_ipc_config;
    memcpy(dst, src, sizeof(struct ipc_server_register_cb_config));
}

static inline void dsm_copy_ipc_server_handler_config(void *src_ipc_config, void *dst_ipc_config)
{
    struct ipc_server_handler_config *src =
            (struct ipc_server_handler_config *)src_ipc_config;
    struct ipc_server_handler_config *dst =
            (struct ipc_server_handler_config *)dst_ipc_config;
    memcpy(dst, src, sizeof(struct ipc_server_handler_config));

    if (src->active_conn) {
        // TODO: copy the active_conn
    } else {
        dst->active_conn = NULL;
    }
}

static inline void dsm_copy_ipc_server_config(void *src_ipc_config, void *dst_ipc_config)
{
    struct ipc_server_config *src =
            (struct ipc_server_config *)src_ipc_config;
    struct ipc_server_config *dst =
            (struct ipc_server_config *)dst_ipc_config;
    memcpy(dst, src, sizeof(struct ipc_server_config));

    if (src->register_cb_thread) {
        // TODO: copy the register_cb_thread
    } else {
        dst->register_cb_thread = NULL;
    }
}

static inline int dsm_copy_ipc_config(void *src_ipc_config, void *dst_ipc_config, mem_t mem_type)
{
    switch (((struct ipc_config *)src_ipc_config)->config_type) {
    case IPC_SERVER_REGISTER_CB: {
        dst_ipc_config = kmalloc(sizeof(struct ipc_server_register_cb_config), mem_type);
        if (!dst_ipc_config) {
            return -ENOMEM;
        }
        dsm_copy_ipc_server_register_cb_config(src_ipc_config, dst_ipc_config);
        break;
    }
    case IPC_SERVER_HANDLER: {
        dst_ipc_config = kmalloc(sizeof(struct ipc_server_handler_config), mem_type);
        if (!dst_ipc_config) {
            return -ENOMEM;
        }
        dsm_copy_ipc_server_handler_config(src_ipc_config, dst_ipc_config);
        break;
    }
    case IPC_SERVER: {
        dst_ipc_config = kmalloc(sizeof(struct ipc_server_config), mem_type);
        if (!dst_ipc_config) {
            return -ENOMEM;
        }
        dsm_copy_ipc_server_config(src_ipc_config, dst_ipc_config);
        break;
    }
    default:
        BUG("Invalid IPC config type\n");
    }
    return 0;
}

int dsm_copy_thread(struct object *src_obj, struct object *dst_obj)
{
    struct thread *src_thread = (struct thread *)src_obj->opaque;
    struct thread *dst_thread = (struct thread *)dst_obj->opaque;
    int is_demote = is_private_object(src_obj);
    int ret = 0;

    /* Copy thread context */
    memcpy(&dst_thread->thread_ctx, &src_thread->thread_ctx, sizeof(struct thread_ctx));

    /* Copy sleep state */
    dst_thread->sleep_state.cb = src_thread->sleep_state.cb;
    dst_thread->sleep_state.sleep_cpu = src_thread->sleep_state.sleep_cpu;
    dst_thread->sleep_state.wakeup_tick = src_thread->sleep_state.wakeup_tick;
    dst_thread->sleep_state.pending_notific = NULL; /* Will be set later */

    /* Copy IPC config */
    if (src_thread->general_ipc_config) {
        ret = dsm_copy_ipc_config(src_thread->general_ipc_config, dst_thread->general_ipc_config, 
            is_demote ? __MT_SHARED__ : __MT_PRIVATE__);
        if (ret) 
            return ret;
    } else {
        dst_thread->general_ipc_config = NULL;
    }

    return 0;
}
