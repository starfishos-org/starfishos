#ifndef __DSM_TIERING_H__
#define __DSM_TIERING_H__

#include <object/object.h>
#include <common/types.h>
#include <common/lock.h>
#include <dsm/dsm-mmconfig.h>
#include <mm/vmspace.h>
#include <dsm/perf_timing.h>

static inline struct object *
dsm_get_inuse_object(struct object *obj, bool waiting)
{
    switch (obj->status) {
    case DSM_STATUS_INVALID:
        return (obj->pair_obj && obj->pair_obj->status == DSM_STATUS_INUSE) ? 
            obj->pair_obj : NULL;
    case DSM_STATUS_INUSE:
        return obj;
    case DSM_STATUS_MIGRATING:
        if (!waiting) return NULL;
        /* waiting for the object to be migrated */
        while (obj->status != DSM_STATUS_MIGRATED);
    case DSM_STATUS_MIGRATED:
        BUG_ON(!obj->pair_obj || obj->pair_obj->status != DSM_STATUS_INUSE);
        return obj->pair_obj;
    default:
        BUG("invalid object status");
    }
}

static inline int
dsm_alloc_pair_object(struct object *obj, mem_t mem_type)
{
    struct object *target;
    
    target = obj->pair_obj;
    if (!target) {
        target = object_alloc(obj->type, obj->size, mem_type);
        if (!target) {
            return -ENOMEM;
        }
    }

    target->pair_obj = obj;
    obj->pair_obj = target;
    return 0;
}

static inline struct object *
dsm_get_object_by_mem_type(struct object *obj, mem_t mem_type, bool alloc)
{
    // BUG_ON(!IS_VALID_MEM_TYPE(mem_type));
    if (obj->mem_type == mem_type) {
        return obj;
    }
    struct object *ret_obj = obj->pair_obj;
    /* alloc a new object if not exist and @alloc is true */
    if (!ret_obj) {
        if (!alloc) return NULL;
        int ret = dsm_alloc_pair_object(obj, mem_type);
        if (ret) return NULL;
        write_lock(&obj->tiering_lock);
        ret_obj = obj->pair_obj;
        write_unlock(&obj->tiering_lock);
    }

    /* check memory type of the pair object */
    if (ret_obj->mem_type == mem_type) {
        return ret_obj;
    }
    return NULL;
}

/* demote/promote the object to the higher/lower tier */
int dsm_demote_object(struct object *obj);
int dsm_promote_object(struct object *obj);

int stop_all_threads(struct list_head *thread_list);
int start_all_threads(struct list_head *thread_list);

int dsm_migrate_process_prepare(struct object *root_cg_obj);
int dsm_migrate_process_ckpt(struct object *root_cg_obj);
int dsm_migrate_process_restore(struct cap_group *new_cap_group);

int dsm_demote_page(struct vmspace *vmspace, void *dst_va, void *src_va, bool retry);

#endif /* __DSM_TIERING_H__ */