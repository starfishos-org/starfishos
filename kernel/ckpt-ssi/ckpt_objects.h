#pragma once

#include <arch/mmu.h>
#include <ckpt/ckpt.h>
#include <ckpt/ckpt_data.h>
#include <common/kvstore.h>
#include <common/util.h>
#include <object/thread.h>
#include <object/cap_group.h>
#include <irq/timer.h>
#include <mm/mm.h>
#include <mm/nvm.h>
#include <mm/kmalloc.h>
#include <mm/uaccess.h>
#include <mm/rmap.h>
#include <sched/context.h>
#include <sched/fpu.h>
#include <perf/measure.h>
#include <ipc/connection.h>
#include <ckpt/hot_pages_tracker.h>

/* Copy policy */
int cap_group_copy_ckpt(struct cap_group *cap_group,
                        struct ckpt_cap_group *ckpt_cap_group);
int cap_group_restore(struct object *, struct ckpt_object *,
                      struct kvs *obj_map, bool time_traveling);
int ckpt_cap_group_copy(struct ckpt_object *src_obj,
                        struct ckpt_object *dst_obj, struct kvs *);

int thread_ckpt(struct thread *target, struct ckpt_thread *ckpt_thread);
int thread_restore(struct object *, struct ckpt_object *, struct kvs *obj_map,
                   bool time_traveling);
int ckpt_thread_copy(struct ckpt_object *src, struct ckpt_object *dst,
                     struct kvs *);

int vmspace_ckpt(struct vmspace *target_vmspace,
                 struct ckpt_vmspace *ckpt_vmspace);
int vmspace_restore(struct object *, struct ckpt_object *, struct kvs *obj_map,
                    bool time_traveling);
int ckpt_vmspace_copy(struct ckpt_object *src_obj, struct ckpt_object *dst_obj,
                      struct kvs *obj_map);

int connection_ckpt(struct ipc_connection *conn,
                    struct ckpt_ipc_connection *ckpt_conn, int alloc);
int connection_restore(struct object *, struct ckpt_object *,
                       struct kvs *obj_map, bool time_traveling);
int ckpt_connection_copy(struct ckpt_object *src_obj,
                         struct ckpt_object *dst_obj, struct kvs *obj_map);

int notification_ckpt(struct notification *notifc,
                      struct ckpt_notification *ckpt_notifc, int alloc);
int notification_restore(struct object *, struct ckpt_object *,
                         struct kvs *obj_map, bool time_traveling);
int ckpt_notification_copy(struct ckpt_object *src_obj,
                           struct ckpt_object *dst_obj, struct kvs *obj_map);

int irq_ckpt(struct irq_notification *irq_notifc,
             struct ckpt_irq_notification *ckpt_irq_notifc, int alloc);
int irq_restore(struct object *, struct ckpt_object *, struct kvs *obj_map,
                bool time_traveling);
int ckpt_irq_copy(struct ckpt_object *src_obj, struct ckpt_object *dst_obj,
                  struct kvs *obj_map);

int pmo_ckpt(struct pmobject *pmo, struct ckpt_pmobject *ckpt_pmo);
int pmo_restore(struct object *, struct ckpt_object *, struct kvs *obj_map,
                bool time_traveling);
int ckpt_pmo_copy(struct ckpt_object *src_obj, struct ckpt_object *dst_obj,
                  struct kvs *obj_map);
