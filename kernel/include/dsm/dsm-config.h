#include <common/util.h>
#include <object/cap_group.h>
#include <object/thread.h>

inline static bool is_system_services(struct cap_group *cg)
{
    // if end with .srv, it is a system service
    if (strstr(cg->cap_group_name, "srv") != NULL) {
        return true;
    }

    if (strncmp(cg->cap_group_name, "chcore_shell.bin", 18) == 0) {
        return true;
    }

    return false;
}

inline static bool is_system_services_thread(struct thread *thread)
{
    if (thread->thread_ctx->type == TYPE_SERVICES 
        || (is_system_services(thread->cap_group) && thread->thread_ctx->type != TYPE_SHADOW)) {
        return true;
    }
    return false;
}

inline static bool is_local_notification(struct notification *notification)
{
    return true;
}

inline static bool is_system_services_ipc_connection(struct ipc_connection *conn)
{
    return is_system_services_thread(conn->server_handler_thread);
}

inline static bool is_system_services_object(struct object *obj)
{
    int ret = false;

    // if (obj->dsm_type == DSM_TYPE_CROSS_SHARED) {
    //     return true;
    // }

    switch (obj->type) {
        case TYPE_CAP_GROUP:
            ret = is_system_services((struct cap_group *)obj->opaque);
            break;
        case TYPE_THREAD:
            ret = is_system_services_thread((struct thread *)obj->opaque);
            break;
        // case TYPE_NOTIFICATION:
        //     ret = !is_local_notification((struct notification *)obj->opaque);
        //     break;
        // case TYPE_CONNECTION:
        //     ret = is_system_services_ipc_connection((struct ipc_connection *)obj->opaque);
        //     break;
        default:
            break;
    }

    // if (ret) {
    //     obj->dsm_type = DSM_TYPE_CROSS_SHARED;
    // }
    return ret;
}
