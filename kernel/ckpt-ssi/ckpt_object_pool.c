#include <common/kvstore.h>
#include <common/errno.h>
#include <common/util.h>
#include <object/thread.h>
#include <ckpt/ckpt_data.h>
#include <ckpt/ckpt.h>
#include <arch/mmu.h>
#include <object/cap_group.h>
#include <mm/mm.h>
#include <mm/kmalloc.h>
#include <mm/nvm.h>
#include <dsm/dsm-single.h>

#include "ckpt_object_pool.h"
#include "ckpt_objects.h"
#include "ckpt_ws.h"

const obj_restore_func obj_restore_tbl[TYPE_NR] = {
        [0 ... TYPE_NR - 1] = NULL,
        [TYPE_CAP_GROUP] = cap_group_restore,
        [TYPE_THREAD] = thread_restore,
        [TYPE_CONNECTION] = connection_restore,
        [TYPE_NOTIFICATION] = notification_restore,
        [TYPE_IRQ] = irq_restore,
        [TYPE_PMO] = pmo_restore,
        [TYPE_VMSPACE] = vmspace_restore,
};

struct ckpt_ws_data *init_ckpt_ws_data()
{
    struct ckpt_ws_data *data;
    data = kzalloc(sizeof(*data), __SHARED__);
    if (!data) {
        return NULL;
    }
    return data;
}

struct ckpt_ws_data *get_ckpt_ws_data()
{
    struct ckpt_ws_data *data;
    extern struct ckpt_ws_data *second_latest_ws_data;
    extern struct ckpt_ws_data *latest_ws_data;
    if (likely(second_latest_ws_data)) {
        data = second_latest_ws_data;
        /* update data version number */
        data->version_number = data->version_number + 2;
    } else {
        data = init_ckpt_ws_data();
        if (!data) {
            goto out_fail;
        }

        /* initial data version number */
        if (latest_ws_data) {
            data->version_number = latest_ws_data->version_number + 1;
        } else {
            data->version_number = 1;
        }
    }

    return data;

out_fail:
    return NULL;
}

int ckpt_obj_map_init(void)
{
    if (CKPT_GLOBAL_OBJ_MAP) {
        return 0;
    }

    CKPT_GLOBAL_OBJ_MAP = new_kvs(KVS_SIZE);
    if (!CKPT_GLOBAL_OBJ_MAP) {
        kinfo("[CKPT OBJ MAP] can not alloc global_obj_map\n");
        return -ENOMEM;
    }

    return 0;
}

static u64 ckpt_obj_size[TYPE_NR] = {sizeof(struct ckpt_cap_group),
                                     sizeof(struct ckpt_thread),
                                     sizeof(struct ckpt_ipc_connection),
                                     sizeof(struct ckpt_notification),
                                     sizeof(struct ckpt_irq_notification),
                                     sizeof(struct ckpt_pmobject),
                                     sizeof(struct ckpt_vmspace)};

struct ckpt_object *ckpt_obj_alloc(u64 type)
{
    u64 total_size;
    struct ckpt_object *object;
    BUG_ON(type >= TYPE_NR);
    // XXX: opaque is u64 so sizeof(*object) is 8-byte aligned.
    //      Thus the address of object-defined data is always 8-byte
    //      aligned.
    total_size = sizeof(*object) + ckpt_obj_size[type];
    object = kzalloc(total_size, __SHARED__);
    /* TODO: errno ecoded in pointer */
    if (!object)
        return NULL;

    object->type = type;
    return object;
}

struct ckpt_obj_root *ckpt_obj_root_alloc()
{
    struct ckpt_obj_root *root;
    root = kmalloc(sizeof(*root), __SHARED__);
    if (!root)
        return NULL;

#if OBJ_OVERWRITE == 1
    root->ckpt_objs[0] = NULL;
    root->ckpt_objs[1] = NULL;
#endif
    root->cow = false;
    root->refcnt = 0;
    root->flip_flag = system_current_flip_flag ^ 1;

    return root;
}

struct ckpt_obj_root *ckpt_obj_root_get(struct object *obj, int alloc)
{
    if (!CKPT_GLOBAL_OBJ_MAP || !obj) {
        BUG("the global object map or the obj is NULL");
        return NULL;
    }

    struct ckpt_obj_root *root = obj->obj_root;
    if (!root && alloc) {
        root = ckpt_obj_root_alloc();
        obj->obj_root = root;
        root->obj = obj;
    }

    return root;
}

#ifdef REPORT
extern int eval_obj_count[];
#endif
#ifdef REPORT
extern u64 eval_obj_time[];
extern u64 get_second_latest_obj_time;
#endif
/*
 * if alloc == true, we will checkpoint the object
 */
struct ckpt_object *ckpt_obj_get(struct ckpt_obj_root *ckpt_obj_root, int alloc)
{
    int r;
    struct object *obj = NULL;
    struct ckpt_object *ckpt_obj = NULL;
    int current_ckpt_verison = get_current_ckpt_version();

    /* check the flip-flag first */
    if (system_current_flip_flag == ckpt_obj_root->flip_flag) {
        /* this object has been checkpointed */
        /* TODO(MOK): if we use COW method, we should get latest ckpt obj */
        BUG_ON(!(ckpt_obj = get_second_latest_ckpt_obj(ckpt_obj_root,
                                                       current_ckpt_verison)));
        return ckpt_obj;
    }

    if (likely(alloc)) {
#ifdef REPORT
        DECLTMR;
        start();
#endif
        ckpt_obj_root->flip_flag = system_current_flip_flag;
        obj = ckpt_obj_root->obj;
#ifdef REPORT
        eval_obj_count[obj->type]++;
#endif
        if (ckpt_obj_root->cow == true) {
            BUG_ON(1);
            /* reuse the latest object because it has not been modified */
            ckpt_obj = get_latest_ckpt_obj(ckpt_obj_root, current_ckpt_verison);

            /* TODO: add the reference count of object node
             * if it is referenced by multiple checkpoints.
             * if we use only one checkpoint,
             * the reference count is useless.
             */

            ckpt_obj_root->refcnt++;
            return ckpt_obj;
        }

        /* we may reuse the second latest object node for checkpoint */
        if (!(ckpt_obj = get_second_latest_ckpt_obj(ckpt_obj_root,
                                                    current_ckpt_verison))) {
            ckpt_obj = ckpt_obj_alloc(obj->type);
            if (!ckpt_obj) {
                goto out_fail;
            }
            set_second_latest_ckpt_obj(
                    ckpt_obj_root, current_ckpt_verison, ckpt_obj);
        }

        ckpt_obj_root->refcnt++;
#ifdef REPORT
        u64 object_time_begin = plat_get_mono_time();
        get_second_latest_obj_time += object_time_begin - timer_start;
#endif
        char *ckpt_name = "/test_hello.bin";
        switch (obj->type) {
        case TYPE_PMO: {
            pmo_ckpt((struct pmobject *)obj->opaque,
                     (struct ckpt_pmobject *)ckpt_obj->opaque);
            break;
        }
        case TYPE_CAP_GROUP: {
            r = cap_group_copy_ckpt((struct cap_group *)obj->opaque,
                                    (struct ckpt_cap_group *)ckpt_obj->opaque);
            if (r) {
                goto out_fail;
            }
            break;
        }
        case TYPE_THREAD: {
#ifdef OMIT_BENCHMARK
            char *name =
                    ((struct thread *)obj->opaque)->cap_group->cap_group_name;
            if (!strcmp(name, "/ycsbc") || !strcmp(name, "/redis_benchmark")
                || !strcmp(name, "/memcachetest"))
                break;
#endif
            struct thread *thread = (struct thread *)obj->opaque;
            if (strcmp(thread->cap_group->cap_group_name, ckpt_name) == 0) {
                thread->thread_ctx->state = TS_WAITING;
                sched();
            }
            thread_ckpt((struct thread *)obj->opaque,
                        (struct ckpt_thread *)ckpt_obj->opaque);

            if (strcmp(thread->cap_group->cap_group_name, ckpt_name) == 0) {
                kdebug("[%s] thread %s enqueue\n",
                       __func__,
                       thread->cap_group->cap_group_name);
                dsm_enqueue(ckpt_obj);
            }
            break;
        }
        case TYPE_CONNECTION: {
            r = connection_ckpt((struct ipc_connection *)obj->opaque,
                                (struct ckpt_ipc_connection *)ckpt_obj->opaque,
                                true);
            if (r) {
                goto out_fail;
            }
            break;
        }
        case TYPE_NOTIFICATION: {
            r = notification_ckpt((struct notification *)obj->opaque,
                                  (struct ckpt_notification *)ckpt_obj->opaque,
                                  true);
            if (r) {
                goto out_fail;
            }
            break;
        }
        case TYPE_IRQ: {
            r = irq_ckpt((struct irq_notification *)obj->opaque,
                         (struct ckpt_irq_notification *)ckpt_obj->opaque,
                         true);
            if (r) {
                goto out_fail;
            }
            break;
        }
        case TYPE_VMSPACE: {
            r = vmspace_ckpt((struct vmspace *)obj->opaque,
                             (struct ckpt_vmspace *)ckpt_obj->opaque);
            if (r) {
                goto out_fail;
            }
            break;
        }
        default:
            /* Shouldn't reach here */
            BUG_ON(1);
        }
#ifdef REPORT
        if (obj->type != TYPE_CAP_GROUP) {
            eval_obj_time[obj->type] +=
                    plat_get_mono_time() - object_time_begin;
        }
#endif
    }
    // printk("[return]ckpt_obj_node_get: type=%d,obj=%p\n",obj->type,obj);
    return ckpt_obj;
out_fail:
    return NULL;
}

inline struct object *restore_obj_get(struct ckpt_obj_root *ckpt_obj_root)
{
    return ckpt_obj_root->obj;
}

/*
 * if alloc == true, we will restore the object
 */
struct object *restore_obj_get_by_cap_group(struct ckpt_obj_root *ckpt_obj_root,
                                            struct kvs *obj_map, int alloc)
{
    obj_restore_func func;
    struct object *obj = NULL;
    struct ckpt_object *ckpt_obj;
    struct object **obj_value =
            (struct object **)kvs_get(obj_map, (kvs_key_t *)&ckpt_obj_root);

    if (obj_value) {
        /* object is already allocated */
        obj = *obj_value;
    } else if (alloc) {
        ckpt_obj_root->flip_flag = system_current_flip_flag;

        /* get ckpt obj */
        if (ckpt_obj_root->time_traveling) {
            /* time traveling obj always use the first ckpt_obj */
            ckpt_obj = ckpt_obj_root->ckpt_objs[0];
        } else {
            ckpt_obj = get_second_latest_ckpt_obj(ckpt_obj_root,
                                                  get_current_ckpt_version());
        }
        BUG_ON(!ckpt_obj);
        obj = ckpt_obj_root->obj;
        kvs_put(obj_map, (kvs_key_t *)&ckpt_obj_root, (kvs_value_t *)(&obj));
#ifdef RESTORE_REPORT
        DECLTMR;
        start();
#endif
        /* Invoke the object-specific restore routine */
        kdebug("restore obj type %d\n", obj->type);
        func = obj_restore_tbl[obj->type];
        if (func) {
            BUG_ON(func(obj, ckpt_obj, obj_map, ckpt_obj_root->time_traveling));
        } else {
            WARN("obj restore func is NULL");
            BUG_ON(1);
        }
#ifdef RESTORE_REPORT
        if (obj->type != TYPE_CAP_GROUP)
            eval_restore_obj_time[obj->type] += stop();
        eval_restore_obj_count[obj->type]++;
#endif
    }
    return obj;
}

inline struct ckpt_object *
get_latest_ckpt_obj(struct ckpt_obj_root *ckpt_obj_root, u64 version_number)
{
    return ckpt_obj_root->ckpt_objs[version_number % 2];
}

inline struct ckpt_object *
get_second_latest_ckpt_obj(struct ckpt_obj_root *ckpt_obj_root,
                           u64 version_number)
{
    return ckpt_obj_root->ckpt_objs[1 - version_number % 2];
}

inline void set_latest_ckpt_obj(struct ckpt_obj_root *ckpt_obj_root,
                                u64 version_number,
                                struct ckpt_object *ckpt_obj)
{
    ckpt_obj_root->ckpt_objs[version_number % 2] = ckpt_obj;
}

inline void set_second_latest_ckpt_obj(struct ckpt_obj_root *ckpt_obj_root,
                                       u64 version_number,
                                       struct ckpt_object *ckpt_obj)
{
    ckpt_obj_root->ckpt_objs[1 - version_number % 2] = ckpt_obj;
}

void dsm_enqueue(void *data)
{
    struct shared_queue_meta *queue;
    struct dsm_queue_node *queue_node;

    queue_node = kmalloc(sizeof(*queue_node), __SHARED__);
    queue_node->data = data;

    queue = &dsm_meta->ready_to_merge_object_queue;
    lock(&(queue->queue_lock));
    list_append(&(queue_node->node), &(queue->queue_head));
    queue->queue_len++;
    unlock(&(queue->queue_lock));
}

void *dsm_dequeue()
{
    struct shared_queue_meta *queue;
    struct list_head *pos;
    void *data;

    queue = &dsm_meta->ready_to_merge_object_queue;
    lock(&(queue->queue_lock));
    if (list_empty(&(queue->queue_head))) {
        data = NULL;
        goto out;
    }
    pos = queue->queue_head.next;
    data = container_of(pos, struct dsm_queue_node, node)->data;

    list_del(pos);
    queue->queue_len--;
out:
    unlock(&(queue->queue_lock));
    return data;
}
