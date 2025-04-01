#include <common/util.h>
#include <object/cap_group.h>

inline static bool is_system_services(struct cap_group *cg)
{
    // if end with .srv, it is a system service
    if (strstr(cg->cap_group_name, "srv") != NULL) {
        return true;
    }

    if (strncmp(cg->cap_group_name, "chcore_shell.bin", 18) == 0) {
        return true;
    }

    kinfo("is_cross_shared_obj: cap group %s\n", cg->cap_group_name);

    return false;
}

inline static bool is_system_services_thread(struct thread *thread)
{
    // if (!thread->general_ipc_config) {
    //     return false;
    // }
    // struct ipc_config *ipc_config = (struct ipc_config *)thread->general_ipc_config;
    // return (ipc_config->config_type == IPC_SERVER);
    if (thread->general_ipc_config != NULL) {
        kinfo("is_cross_shared_obj: server thread %s\n", thread->cap_group->cap_group_name);
        return true;
    }
    return false;
}

inline static bool is_cross_shared_obj(struct object *obj)
{
    switch (obj->type) {
        case TYPE_CAP_GROUP:
            return is_system_services((struct cap_group *)obj->opaque);
        case TYPE_THREAD:
            return is_system_services_thread((struct thread *)obj->opaque);
        // case TYPE_NOTIFICATION:
        //     kinfo("notification: %p\n", obj);
        //     return true;
        // case TYPE_CONNECTION:
        //     kinfo("ipc connection: %p\n", obj);
        //     return true;
        default:
            break;
    }
    return false;
}
