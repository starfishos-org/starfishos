#pragma once

#include <common/types.h>
#include <common/errno.h>
#include <common/lock.h>
#include <common/list.h>
#include <common/lock.h>
#include <common/kvstore.h>
#include <dsm/dsm-mmconfig.h>

struct object {
    u64 type;
    u64 size;
    /* Link all slots point to this object */
    struct list_head copies_head;
    /* Currently only protect copies list */
    struct lock copies_lock;
    /*
     * refcount is added when a slot points to it and when get_object is
     * called. Object is freed when it reaches 0.
     */
    u64 refcount;
    /*
     * we use obj_root for checkpoint
     */
    struct ckpt_obj_root *obj_root;
#ifdef DSM_ENABLED
    /*
     * Memory type of the object.
     */
    mem_t mem_type;
    mid_t machine_id; // created machine id
    /*
     * For migrating objects, dirty_bit is used to indicate the object is dirty.
     */
#define DSM_STATUS_INVALID     (0) // the object is invalid
#define DSM_STATUS_INUSE       (1) // the object is in use
#define DSM_STATUS_MIGRATING   (2) // the object is being migrated
#define DSM_STATUS_MIGRATED    (3) // the object is migrated
    u8 status;

    /*
     * Three cases for object tiering
     * 1. DSM_TYPE_NORMAL: normal object that can be used
     * 2. DSM_TYPE_BRIDGE: a shared object for two machines to connect; should *    never be used, and old private obj will be deleted
     *    e.g., thread: private -> bridge -> private
     * 3. DSM_TYPE_CROSS_SHARED: a cross-shared object that can be used
     *    e.g., ipc, notification shared by many threads
     *    e.g., root_cap_group: private old machine's root_cap_group -> bridge  *          -> private new machine's root_cap_group
     */
#define DSM_TYPE_NORMAL         (0) 
#define DSM_TYPE_BRIDGE         (1)
#define DSM_TYPE_CROSS_SHARED   (2)
// notify this object, will wake up the true thread on its machine
#define DSM_TYPE_THREAD_NOTIFY_BRIDGE (3)
    u8 dsm_type;
    
    u8 dirty_bit;  // the object is dirty during migration
    /*
     * For tiering, link the object to another object.
     */
    struct object *pair_obj;
    /*
     * Lock for migration.
     */
    struct lock tiering_lock;
#endif
     /* 
     * opaque marks the end of this struct and the real object will be
     * stored here. Now its address will be 8-byte aligned.
     */
    u64 opaque[];
};

enum object_type {
    TYPE_CAP_GROUP = 0,
    TYPE_THREAD,
    TYPE_CONNECTION,
    TYPE_NOTIFICATION,
    TYPE_IRQ,
    TYPE_PMO,
    TYPE_VMSPACE,
#ifdef CHCORE_KERNEL_VIRT
    TYPE_VM,
    TYPE_VCPU,
    TYPE_IPA_REGION,
#endif /* CHCORE_KERNEL_VIRT */
    TYPE_NR,
};

extern const char* obj_name_tbl[TYPE_NR];

struct cap_group;

typedef void (*obj_deinit_func)(void *);
extern const obj_deinit_func obj_deinit_tbl[TYPE_NR];

#define is_private_object(obj) ((obj)->mem_type == __MT_PRIVATE__)
#define is_shared_object(obj) ((obj)->mem_type == __MT_SHARED__)

#define obj2object(obj) (container_of(obj, struct object, opaque))
#define object2obj(object) (object->opaque)
#define obj2objpair(obj) (object2obj(obj2object(obj)->pair_obj))

void *obj_get(struct cap_group *cap_group, int slot_id, int type);
void obj_put(void *obj);

struct object *object_alloc(u64 type, u64 size, mem_t flags);
void *obj_alloc(u64 type, u64 size, mem_t flags);
void obj_free(void *obj);
int cap_alloc(struct cap_group *cap_group, void *obj, u64 rights);
int cap_free(struct cap_group *cap_group, int slot_id);
int cap_copy(struct cap_group *src_cap_group, struct cap_group *dest_cap_group,
             int src_slot_id);
int cap_move(struct cap_group *src_cap_group, struct cap_group *dest_cap_group,
             int src_slot_id);

int cap_free_all(struct cap_group *cap_group, int slot_id);

/* Syscalls */
int sys_cap_copy_to(u64 dest_cap_group_cap, u64 src_slot_id);
int sys_cap_copy_from(u64 src_cap_group_cap, u64 src_slot_id);
int sys_transfer_caps(u64 dest_group_cap, u64 src_caps_buf, int nr_caps,
                      u64 dst_caps_buf);
int sys_cap_move(u64 dest_cap_group_cap, u64 src_slot_id);
int sys_get_all_caps(u64 cap_group_cap);
int sys_revoke_cap(u64 obj_cap);

int cap_insert(struct cap_group *cap_group, struct object *object, u64 rights, int slot_id, mem_t mem_type);
int cap_replace(struct cap_group *cap_group, struct object *object, int slot_id);
