#ifndef __DSM_TIERING_H__
#define __DSM_TIERING_H__

#include <object/object.h>
#include <common/types.h>
#include <common/lock.h>
#include <dsm/dsm-mmconfig.h>

static inline struct object *
dsm_tiering_get_object(struct object *obj, bool waiting)
{
    switch (obj->status) {
        case DSM_STATUS_INVALID:
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

/* demote/promote the object to the higher/lower tier */
int dsm_demote_object(struct object *obj);
int dsm_promote_object(struct object *obj);

#endif /* __DSM_TIERING_H__ */