#include <common/kvstore.h>
#include <sched/sched.h>
#include "../ckpt_objects.h"
#include "../ckpt_object_pool.h"

void ckpt_ipc_config_copy(void *src_ipc_config, void *dst_ipc_config,
                          int config_type, struct kvs *obj_map)
{
    switch (config_type) {
    case IPC_SERVER_REGISTER_CB: {
        struct ckpt_ipc_server_register_cb_config *src =
                (struct ckpt_ipc_server_register_cb_config *)src_ipc_config;
        struct ckpt_ipc_server_register_cb_config *dst =
                (struct ckpt_ipc_server_register_cb_config *)dst_ipc_config;
        memcpy(dst, src, sizeof(struct ckpt_ipc_server_register_cb_config));
        break;
    }
    case IPC_SERVER_HANDLER: {
        struct ckpt_ipc_server_handler_config *src =
                (struct ckpt_ipc_server_handler_config *)src_ipc_config;
        struct ckpt_ipc_server_handler_config *dst =
                (struct ckpt_ipc_server_handler_config *)dst_ipc_config;
        struct ckpt_obj_root *ckpt_obj_root;

        memcpy(dst, src, sizeof(struct ckpt_ipc_server_handler_config));

        if (src->active_conn_root) {
            ckpt_obj_root = get_copied_obj_root(src->active_conn_root, obj_map);
            BUG_ON(ckpt_obj_root == NULL);
            dst->active_conn_root = ckpt_obj_root;
        } else {
            dst->active_conn_root = NULL;
        }

        break;
    }
    case IPC_SERVER: {
        struct ckpt_ipc_server_config *src =
                (struct ckpt_ipc_server_config *)src_ipc_config;
        struct ckpt_ipc_server_config *dst =
                (struct ckpt_ipc_server_config *)dst_ipc_config;
        struct ckpt_obj_root *ckpt_thread_obj_root;

        dst->config_type = src->config_type;
        dst->declared_ipc_routine_entry = src->declared_ipc_routine_entry;

        if (src->register_cb_thread_root) {
            ckpt_thread_obj_root =
                    get_copied_obj_root(src->register_cb_thread_root, obj_map);
            BUG_ON(ckpt_thread_obj_root == NULL);
            dst->register_cb_thread_root = ckpt_thread_obj_root;
        } else {
            dst->register_cb_thread_root = NULL;
        }

        break;
    }
    default:
        BUG_ON(1);
    }
}

void *alloc_ckpt_ipc_config(int config_type)
{
    void *ckpt_ipc_config = NULL;
    switch (config_type) {
    case IPC_SERVER_REGISTER_CB: {
        ckpt_ipc_config = kmalloc(
                sizeof(struct ckpt_ipc_server_register_cb_config), __MT_SHARED__);
        break;
    }
    case IPC_SERVER_HANDLER: {
        ckpt_ipc_config = kmalloc(sizeof(struct ckpt_ipc_server_handler_config),
                                  __MT_SHARED__);
        break;
    }
    case IPC_SERVER: {
        ckpt_ipc_config =
                kmalloc(sizeof(struct ckpt_ipc_server_config), __MT_SHARED__);
        break;
    }
    default:
        break;
    }
    return ckpt_ipc_config;
}

static void thread_ctx_ckpt(struct ckpt_thread_ctx *ctx,
                            struct thread_ctx *target_ctx)
{
    int i;
    ctx->ec = target_ctx->ec;
    ctx->affinity = target_ctx->affinity;
    ctx->kernel_stack_state = target_ctx->kernel_stack_state;
    ctx->prio = target_ctx->prio;
    ctx->type = target_ctx->type;
    ctx->thread_exit_state = target_ctx->thread_exit_state;
    ctx->state = target_ctx->state;

    /* Only available for x86 */
    if (target_ctx->fpu_state) {
        if (!ctx->fpu_state) {
            ctx->fpu_state = alloc_fpu_state();
            BUG_ON(!ctx->fpu_state);
        }

        copy_fpu_state(ctx->fpu_state, target_ctx->fpu_state);
    }

    for (i = 0; i < TLS_REG_NUM; i++) {
        ctx->tls_base_reg[i] = target_ctx->tls_base_reg[i];
    }
}

void thread_sleep_state_ckpt(struct ckpt_sleep_state *ckpt_sleep_state,
                             struct sleep_state *sleep_state)
{
    /* Copy the other fields */
    ckpt_sleep_state->cb = sleep_state->cb;
    ckpt_sleep_state->sleep_cpu = sleep_state->sleep_cpu;
    ckpt_sleep_state->wakeup_tick = sleep_state->wakeup_tick;
}

void ipc_config_ckpt(void *config, void *ckpt_config, int config_type, int flags)
{
    switch (config_type) {
    case IPC_SERVER_REGISTER_CB: {
        struct ipc_server_register_cb_config *ipc_config =
                (struct ipc_server_register_cb_config *)config;
        struct ckpt_ipc_server_register_cb_config *ckpt_ipc_config =
                (struct ckpt_ipc_server_register_cb_config *)ckpt_config;
        ckpt_ipc_config->config_type = ipc_config->config_type;
        ckpt_ipc_config->conn_cap_in_client = ipc_config->conn_cap_in_client;
        ckpt_ipc_config->conn_cap_in_server = ipc_config->conn_cap_in_server;
        ckpt_ipc_config->register_cb_entry = ipc_config->register_cb_entry;
        ckpt_ipc_config->register_cb_stack = ipc_config->register_cb_stack;
        ckpt_ipc_config->shm_cap_in_server = ipc_config->shm_cap_in_server;
        ckpt_ipc_config->register_lock = ipc_config->register_lock;
        break;
    }
    case IPC_SERVER_HANDLER: {
        struct ipc_server_handler_config *ipc_config =
                (struct ipc_server_handler_config *)config;
        struct ckpt_ipc_server_handler_config *ckpt_ipc_config =
                (struct ckpt_ipc_server_handler_config *)ckpt_config;
        struct object *old_conn_obj;
        struct ckpt_obj_root *ckpt_conn_obj_root;

        ckpt_ipc_config->config_type = ipc_config->config_type;
        ckpt_ipc_config->ipc_routine_entry = ipc_config->ipc_routine_entry;
        ckpt_ipc_config->ipc_routine_stack = ipc_config->ipc_routine_stack;
        ckpt_ipc_config->ipc_lock = ipc_config->ipc_lock;

        if (ipc_config->active_conn) {
            old_conn_obj = container_of(
                    ipc_config->active_conn, struct object, opaque);
            ckpt_conn_obj_root = ckpt_obj_root_get(old_conn_obj, flags);
            BUG_ON(ckpt_conn_obj_root == NULL);
            BUG_ON(!ckpt_obj_get(ckpt_conn_obj_root, flags));
            ckpt_ipc_config->active_conn_root = ckpt_conn_obj_root;
        } else {
            ckpt_ipc_config->active_conn_root = NULL;
        }

        break;
    }
    case IPC_SERVER: {
        struct ipc_server_config *ipc_config =
                (struct ipc_server_config *)config;
        struct ckpt_ipc_server_config *ckpt_ipc_config =
                (struct ckpt_ipc_server_config *)ckpt_config;
        struct object *old_thread_obj;
        struct ckpt_obj_root *ckpt_thread_obj_root;

        ckpt_ipc_config->config_type = ipc_config->config_type;
        ckpt_ipc_config->declared_ipc_routine_entry =
                ipc_config->declared_ipc_routine_entry;

        if (ipc_config->register_cb_thread) {
            old_thread_obj = container_of(
                    ipc_config->register_cb_thread, struct object, opaque);
            ckpt_thread_obj_root = ckpt_obj_root_get(old_thread_obj, flags);
            BUG_ON(ckpt_thread_obj_root == NULL);
            BUG_ON(!ckpt_obj_get(ckpt_thread_obj_root, flags));
            ckpt_ipc_config->register_cb_thread_root = ckpt_thread_obj_root;
        } else {
            ckpt_ipc_config->register_cb_thread_root = NULL;
        }

        break;
    }
    default:
        BUG_ON(1);
    }
}

int thread_ckpt(struct thread *target, struct ckpt_thread *ckpt_thread, int flags)
{
#ifdef OMIT_BENCHMARK
    if (is_benchmark_thread(target))
        return 0;
#endif
    /* Copy thread_ctx data */
    thread_ctx_ckpt(&ckpt_thread->thread_ctx, target->thread_ctx);

    /* Copy thread sleep-state */
    thread_sleep_state_ckpt(&ckpt_thread->sleep_state, &target->sleep_state);

    /* Copy ipc config */
    if (target->general_ipc_config != NULL) {
        int config_type =
                ((struct ipc_config *)target->general_ipc_config)->config_type;
        struct ipc_config *ckpt_config = NULL;

        ckpt_config = (struct ipc_config *)ckpt_thread->general_ipc_config;
        if (!ckpt_config) {
            /* we can not reuse it if the pointer of config is NULL*/
            ckpt_config = alloc_ckpt_ipc_config(config_type);
        } else if (config_type
                   != ((struct ipc_config *)ckpt_thread->general_ipc_config)
                              ->config_type) {
            /* we can not reuse it if the type of ipc config in checkpoint
             * is different from the target thread*/
            kfree(ckpt_config);
            ckpt_config = alloc_ckpt_ipc_config(config_type);
        }

        ipc_config_ckpt(target->general_ipc_config, ckpt_config, config_type, flags);
        ckpt_thread->general_ipc_config = ckpt_config;
    } else {
        if (ckpt_thread->general_ipc_config) {
            kfree(ckpt_thread->general_ipc_config);
        }
        ckpt_thread->general_ipc_config = NULL;
    }
    /* Ckpt thread->cap_group */
    ckpt_thread->cap_group_root = ckpt_obj_root_get(
            container_of(target->cap_group, struct object, opaque), flags);
    ckpt_thread->vmspace_root = ckpt_obj_root_get(
            container_of(target->vmspace, struct object, opaque), flags);

    CFORK_LOG_DEBUG("ckpt thread %lx from %s, fpu owner=%d, type=%u, rip=%lx, state=%u, cpuid=%u, kernel_stack_state=%u, id=%lx\n",
          target,
          target->cap_group->cap_group_name,
          target->thread_ctx->is_fpu_owner,
          target->thread_ctx->type,
          target->thread_ctx->ec.reg[RIP],
          target->thread_ctx->state,
          target->thread_ctx->cpuid,
          target->thread_ctx->kernel_stack_state,
          ckpt_obj_root_get(container_of(target, struct object, opaque),
                            flags));
    // kprint_vmr(target->vmspace);
    return 0;
}

static void thread_ctx_restore(struct ckpt_thread_ctx *ckpt_ctx,
                               struct thread_ctx *target_ctx)
{
    int i;
    BUG_ON(ckpt_ctx == NULL || target_ctx == NULL);

    target_ctx->affinity = ckpt_ctx->affinity;
    // target_ctx->kernel_stack_state = ckpt_ctx->kernel_stack_state;
    target_ctx->kernel_stack_state = KS_FREE;
    target_ctx->prio = ckpt_ctx->prio;
    // kinfo("[%s] state %d\n", __func__, ckpt_ctx->state);
    target_ctx->state = ckpt_ctx->state;
    target_ctx->type = ckpt_ctx->type;
    target_ctx->thread_exit_state = ckpt_ctx->thread_exit_state;
    // target_ctx->fpu_state = ckpt_ctx->fpu_state;
    target_ctx->is_fpu_owner = -1;

    for (i = 0; i < TLS_REG_NUM; i++) {
        target_ctx->tls_base_reg[i] = ckpt_ctx->tls_base_reg[i];
    }

    for (i = 0; i < REG_NUM; i++) {
        target_ctx->ec.reg[i] = ckpt_ctx->ec.reg[i];
    }

    /* Set the budget of the thread */
    /* There may cause memory leak for TYPE_SHADOW thread*/
    if (!target_ctx->sc) {
        target_ctx->sc = kmalloc(sizeof(sched_cont_t), __MT_SHARED__);
        target_ctx->sc->budget = DEFAULT_BUDGET;
    }
    if (ckpt_ctx->fpu_state) {
        copy_fpu_state(target_ctx->fpu_state, ckpt_ctx->fpu_state);
    }
}

void thread_sleep_state_restore(struct ckpt_sleep_state *ckpt_sleep_state,
                                struct sleep_state *sleep_state,
                                struct kvs *obj_map)
{
    /* Copy fields */
    sleep_state->cb = ckpt_sleep_state->cb;
    sleep_state->sleep_cpu = ckpt_sleep_state->sleep_cpu;
    sleep_state->wakeup_tick = ckpt_sleep_state->wakeup_tick;

    /* Add to wait-queue of the target core if it's sleeping */
    if (sleep_state->cb) {
        sleep_state_enqueue(sleep_state);
    }
    lock_init(&sleep_state->queue_lock);
}

struct ipc_config *ipc_config_restore(struct ipc_config *general_ckpt_ipc_config, 
                                    struct kvs *obj_map, int flags)
{
    switch (general_ckpt_ipc_config->config_type) {
    case IPC_SERVER_REGISTER_CB: {
        struct ckpt_ipc_server_register_cb_config *ckpt_config =
                (struct ckpt_ipc_server_register_cb_config *)general_ckpt_ipc_config;
        struct ipc_server_register_cb_config *new_config =
                kmalloc(sizeof(*new_config), __MT_SHARED__);
        
        new_config->config_type = ckpt_config->config_type;
        new_config->conn_cap_in_client = ckpt_config->conn_cap_in_client;
        new_config->conn_cap_in_server = ckpt_config->conn_cap_in_server;
        new_config->register_cb_entry = ckpt_config->register_cb_entry;
        new_config->register_cb_stack = ckpt_config->register_cb_stack;
        new_config->shm_cap_in_server = ckpt_config->shm_cap_in_server;
        new_config->register_lock = ckpt_config->register_lock;
        return (struct ipc_config *)new_config;
    }
    case IPC_SERVER_HANDLER: {
        struct ckpt_ipc_server_handler_config *ckpt_config =
                (struct ckpt_ipc_server_handler_config *)general_ckpt_ipc_config;
        struct ipc_server_handler_config *new_config =
                kmalloc(sizeof(*new_config), __MT_SHARED__);
        struct object *new_conn_obj;

        CFORK_LOG_DEBUG("[%s] ckpt_config->config_type %d, active_conn_root %p\n", 
            __func__, ckpt_config->config_type, ckpt_config->active_conn_root);

        new_config->config_type = ckpt_config->config_type;
        new_config->ipc_routine_entry = ckpt_config->ipc_routine_entry;
        new_config->ipc_routine_stack = ckpt_config->ipc_routine_stack;
        new_config->ipc_lock = ckpt_config->ipc_lock;
        
        if (ckpt_config->active_conn_root) {
            new_conn_obj = restore_obj_get_by_cap_group(
                            ckpt_config->active_conn_root, obj_map, flags);
            BUG_ON(new_conn_obj == NULL);
            new_config->active_conn = (struct ipc_connection *)new_conn_obj->opaque;
        } else {
            new_config->active_conn = NULL;
        }

        return (struct ipc_config *)new_config;
    }
    case IPC_SERVER: {
        struct ckpt_ipc_server_config *ckpt_config = 
                (struct ckpt_ipc_server_config *)general_ckpt_ipc_config;
        struct ipc_server_config *new_config =
                kmalloc(sizeof(*new_config), __MT_SHARED__);
        struct object *new_thread_obj;

        CFORK_LOG_DEBUG("[%s] ckpt_config->config_type %d, register_cb_thread_root %p\n", 
            __func__, ckpt_config->config_type, ckpt_config->register_cb_thread_root);

        new_config->config_type = ckpt_config->config_type;
        new_config->declared_ipc_routine_entry = ckpt_config->declared_ipc_routine_entry;
        
        if (ckpt_config->register_cb_thread_root) {
            new_thread_obj = restore_obj_get_by_cap_group(
                            ckpt_config->register_cb_thread_root, obj_map, flags);
            BUG_ON(new_thread_obj == NULL);
            new_config->register_cb_thread =
                    (struct thread *)new_thread_obj->opaque;
        } else {
            new_config->register_cb_thread = NULL;
        }

        return (struct ipc_config *)new_config;
    }
    default:
        BUG_ON(1);
    }
}

int thread_restore(struct object *thread_obj,
                   struct ckpt_object *ckpt_thread_obj, 
                   struct kvs *obj_map,
                   int flags)
{
    struct ckpt_thread *ckpt_thread =
            (struct ckpt_thread *)ckpt_thread_obj->opaque;
    struct thread *target = (struct thread *)thread_obj->opaque;
    struct object *cap_obj;
    struct cap_group *thread_cap_group;
    struct vmspace *thread_vmspace;

    // no matter the arg, thread_ctx_restore will handle sc
    target->thread_ctx = create_thread_ctx(ckpt_thread->thread_ctx.type, __MT_THREADCTX__);
    if (!target->thread_ctx) {
        BUG_ON(1);
        return -ENOMEM;
    }
    /* Copy thread_ctx data */
    thread_ctx_restore(&ckpt_thread->thread_ctx, target->thread_ctx);
    /* Restore TLS */
    arch_set_thread_tls(target, ckpt_thread->thread_ctx.tls_base_reg[TLS_FS]);
    /* Copy sleep-state */
    thread_sleep_state_restore(
            &ckpt_thread->sleep_state, &target->sleep_state, obj_map);
    /* Copy ipc config */
    if (ckpt_thread->general_ipc_config != NULL) {
        target->general_ipc_config = ipc_config_restore(
                ckpt_thread->general_ipc_config, obj_map, flags);
    } else {
        target->general_ipc_config = NULL;
    }

    /* Restore cap group */
    BUG_ON(!(cap_obj = restore_obj_get_by_cap_group(ckpt_thread->cap_group_root, obj_map, flags)));
    BUG_ON(!(thread_cap_group = (struct cap_group *)cap_obj->opaque));
    list_add(&target->node, &thread_cap_group->thread_list);
    thread_cap_group->thread_cnt++;
    target->cap_group = thread_cap_group;

    /* Restore vmspace */
    BUG_ON(!(cap_obj = restore_obj_get_by_cap_group(ckpt_thread->vmspace_root, obj_map, flags)));
    BUG_ON(!(thread_vmspace = (struct vmspace *)cap_obj->opaque));
    target->vmspace = thread_vmspace;

    CFORK_LOG_DEBUG("restore thread %lx from %s, type=%u, rip=%lx, rsp=%lx, "
           "state=%u, cpuid=%u, kernel_stack_state=%u, id=%lx\n",
           target,
           thread_cap_group->cap_group_name,
           target->thread_ctx->type,
           target->thread_ctx->ec.reg[RIP],
           target->thread_ctx->ec.reg[RSP],
           target->thread_ctx->state,
           target->thread_ctx->cpuid,
           target->thread_ctx->kernel_stack_state,
           ckpt_obj_root_get(container_of(target, struct object, opaque), flags & ~FLAGS_ALLOC));

    target->prev_thread = NULL;
    // switch (target->thread_ctx->state) {
    // case TS_INIT:
    // case TS_INTER:
    // case TS_RUNNING:
    // case TS_READY: {
    //     target->thread_ctx->state = TS_INTER;
    //     // kinfo("thread %lx enqueue\n",target);
    //     BUG_ON(sched_enqueue(target));
    //     break;
    // }
    // default: {
    //     break;
    // }
    // }

    if (target == current_thread) {
        target->thread_ctx->state = TS_RUNNING;
    }

    return 0;
}

int ckpt_thread_copy(struct ckpt_object *src_obj, struct ckpt_object *dst_obj,
                     struct kvs *obj_map)
{
    struct ckpt_thread *src = (struct ckpt_thread *)src_obj->opaque;
    struct ckpt_thread *dst = (struct ckpt_thread *)dst_obj->opaque;

    /* Copy thread_ctx */
    memcpy(&dst->thread_ctx, &src->thread_ctx, sizeof(struct ckpt_thread_ctx));

    /* Copt thread_ctx->fpu_state */
    dst->thread_ctx.fpu_state = alloc_fpu_state();
    memcpy(dst->thread_ctx.fpu_state,
           src->thread_ctx.fpu_state,
           STATE_AREA_SIZE);

    /* Copy sleep_state */
    memcpy(&dst->sleep_state,
           &src->sleep_state,
           sizeof(struct ckpt_sleep_state));

    /* Copy cap_group_root and vmspace_root */
    dst->cap_group_root = get_copied_obj_root(src->cap_group_root, obj_map);
    dst->vmspace_root = get_copied_obj_root(src->vmspace_root, obj_map);

    /* Copy general_ipc_config */
    if (src->general_ipc_config) {
        int config_type =
                ((struct ipc_config *)src->general_ipc_config)->config_type;
        dst->general_ipc_config = alloc_ckpt_ipc_config(config_type);
        if (!dst->general_ipc_config) {
            return -ENOMEM;
        }
        ckpt_ipc_config_copy(src->general_ipc_config,
                             dst->general_ipc_config,
                             config_type,
                             obj_map);
    } else {
        dst->general_ipc_config = NULL;
    }

    return 0;
}
