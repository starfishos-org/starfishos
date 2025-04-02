#include <common/kvstore.h>
#include <sched/sched.h>
#include <ipc/connection.h>
#include <sched/fpu.h>

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

static void dsm_copy_thread_ctx(struct thread_ctx *src_ctx, struct thread_ctx *dst_ctx, mem_t mem_type)
{
    int i;
    dst_ctx->ec = src_ctx->ec;
    dst_ctx->affinity = src_ctx->affinity;
    dst_ctx->kernel_stack_state = src_ctx->kernel_stack_state;   
    dst_ctx->prio = src_ctx->prio;
    dst_ctx->type = src_ctx->type;
    dst_ctx->thread_exit_state = src_ctx->thread_exit_state;
    dst_ctx->state = src_ctx->state;
    dst_ctx->is_fpu_owner = src_ctx->is_fpu_owner;

    if (src_ctx->sc) {
        if (!dst_ctx->sc) {
            dst_ctx->sc = kzalloc(sizeof(struct sched_cont), mem_type);
            BUG_ON(!dst_ctx->sc);
        }
        dst_ctx->sc->budget = src_ctx->sc->budget;
    }

    /* Only available for x86 */
    if (src_ctx->fpu_state) {
        if (!dst_ctx->fpu_state) {
            dst_ctx->fpu_state = kzalloc(STATE_AREA_SIZE, mem_type);
            BUG_ON(!dst_ctx->fpu_state);
        }

        copy_fpu_state(src_ctx->fpu_state, dst_ctx->fpu_state);
    }

    for (i = 0; i < TLS_REG_NUM; i++) {
        dst_ctx->tls_base_reg[i] = src_ctx->tls_base_reg[i];
    }
}

static void dsm_copy_sleep_state(struct sleep_state *src_sleep_state, struct sleep_state *dst_sleep_state)
{
    /* Copy the other fields */
    dst_sleep_state->cb = src_sleep_state->cb;
    dst_sleep_state->sleep_cpu = src_sleep_state->sleep_cpu;
    dst_sleep_state->wakeup_tick = src_sleep_state->wakeup_tick;
}

int dsm_copy_thread(struct object *src_obj, struct object *dst_obj)
{
    struct thread *src_thread = (struct thread *)src_obj->opaque;
    struct thread *dst_thread = (struct thread *)dst_obj->opaque;
    int is_demote = is_private_object(src_obj);
    mem_t mem_type = is_demote ? __MT_SHARED__ : __MT_PRIVATE__;
    int ret = 0;

    // print_thread(src_thread);
    // kprint_vmr(src_thread->vmspace);

    /* Do not demote server threads */
    if (src_thread->thread_ctx->type == TYPE_SERVICES) {
        DSM_TIER_LOG_DEBUG("server thread %s; skip demote and not add to cap group\n", 
            src_thread->cap_group->cap_group_name);
        return 0;
    }

    /* Copy thread context */
    dst_thread->thread_ctx = kzalloc(sizeof(struct thread_ctx), mem_type);
    BUG_ON(!dst_thread->thread_ctx);
    dsm_copy_thread_ctx(src_thread->thread_ctx, dst_thread->thread_ctx, mem_type);

    /* Copy sleep state */
    dsm_copy_sleep_state(&src_thread->sleep_state, &dst_thread->sleep_state);

    /* Copy IPC config */
    if (src_thread->general_ipc_config) {
        ret = dsm_copy_ipc_config(src_thread->general_ipc_config,       
                    dst_thread->general_ipc_config, 
                    mem_type);
        if (ret) {
            DSM_TIER_LOG_ERR("%s: failed for thread %p\n", __func__, src_obj);
            return ret;
        }
    } else {
        dst_thread->general_ipc_config = NULL;
    }

    /* get the vmspace object */
    struct object *src_vmspace_object = obj2object(src_thread->vmspace);
    struct object *dst_vmspace_object = 
        dsm_get_object_by_mem_type(src_vmspace_object, mem_type, false);
    if (!dst_vmspace_object) {
        DSM_TIER_LOG_ERR("%s: vmspace is not demoted\n", __func__, src_obj);
        return -EINVAL;
    }
    dst_thread->vmspace = (struct vmspace *)object2obj(dst_vmspace_object);

    return 0;
}

int add_thread_to_cap_group(struct thread *dst_thread, struct cap_group *dst_cap_group)
{
    /* add the thread to the dst cap group */
    dst_thread->cap_group = dst_cap_group;
    list_add(&dst_thread->node, &dst_cap_group->thread_list);
    dst_cap_group->thread_cnt++;
    DSM_TIER_LOG_DEBUG("add thread %p to cap group %s\n", 
        dst_thread, dst_cap_group->cap_group_name);
    return 0;
}
