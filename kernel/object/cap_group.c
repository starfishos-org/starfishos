#include <object/cap_group.h>
#include <object/thread.h>
#include <common/list.h>
#include <common/util.h>
#include <common/bitops.h>
#include <mm/kmalloc.h>
#include <mm/vmspace.h>
#include <mm/uaccess.h>
#include <lib/printk.h>
#include <ipc/notification.h>
#include <ckpt/ckpt_data.h>
#include <ckpt/ckpt.h>
#include <sched/context.h>

/* tool functions */
static bool is_valid_slot_id(struct slot_table *slot_table, int slot_id)
{
        if (slot_id < 0 || slot_id >= slot_table->slots_size)
                return false;
        if (!get_bit(slot_id, slot_table->slots_bmp))
                return false;
        if (slot_table->slots[slot_id] == NULL)
                BUG("slot NULL while bmp is not\n");
        return true;
}

static int slot_table_init(struct slot_table *slot_table, unsigned int size,
                           bool init_lock)
{
        int r;

        size = DIV_ROUND_UP(size, BASE_OBJECT_NUM) * BASE_OBJECT_NUM;
        slot_table->slots_size = size;
        /* XXX: vmalloc is better? */
        slot_table->slots = kzalloc(size * sizeof(*slot_table->slots), __DEFAULT__);
        if (!slot_table->slots) {
                r = -ENOMEM;
                goto out_fail;
        }

        slot_table->slots_bmp =
                kzalloc(BITS_TO_LONGS(size) * sizeof(unsigned long), __DEFAULT__);
        if (!slot_table->slots_bmp) {
                r = -ENOMEM;
                goto out_free_slots;
        }

        slot_table->full_slots_bmp = kzalloc(BITS_TO_LONGS(BITS_TO_LONGS(size))
                                             * sizeof(unsigned long), __DEFAULT__);
        if (!slot_table->full_slots_bmp) {
                r = -ENOMEM;
                goto out_free_slots_bmp;
        }

        if (init_lock)
                rwlock_init(&slot_table->table_guard);

        return 0;
out_free_slots_bmp:
        kfree(slot_table->slots_bmp);
out_free_slots:
        kfree(slot_table->slots);
out_fail:
        return r;
}

int cap_group_init(struct cap_group *cap_group, unsigned int size, u64 badge)
{
        struct slot_table *slot_table = &cap_group->slot_table;

        BUG_ON(slot_table_init(slot_table, size, true));
        init_list_head(&cap_group->thread_list);
        lock_init(&cap_group->threads_lock);
        cap_group->thread_cnt = 0;

        /* Set badge of the new cap group. */
        cap_group->badge = badge;

        return 0;
}

void cap_group_deinit(void *ptr)
{
        struct cap_group *cap_group;
        struct slot_table *slot_table;

        cap_group = (struct cap_group *)ptr;
        slot_table = &cap_group->slot_table;
        kfree(slot_table->slots);
        kfree(slot_table->slots_bmp);
        kfree(slot_table->full_slots_bmp);
}

/* slot allocation */
static int expand_slot_table(struct slot_table *slot_table)
{
        unsigned int new_size, old_size;
        struct slot_table new_slot_table;
        int r;

        old_size = slot_table->slots_size;
        new_size = old_size + BASE_OBJECT_NUM;
        r = slot_table_init(&new_slot_table, new_size, false);
        if (r < 0)
                return r;

        memcpy(new_slot_table.slots,
               slot_table->slots,
               old_size * sizeof(*slot_table->slots));
        memcpy(new_slot_table.slots_bmp,
               slot_table->slots_bmp,
               BITS_TO_LONGS(old_size) * sizeof(unsigned long));
        memcpy(new_slot_table.full_slots_bmp,
               slot_table->full_slots_bmp,
               BITS_TO_LONGS(BITS_TO_LONGS(old_size)) * sizeof(unsigned long));
        slot_table->slots_size = new_size;
        kfree(slot_table->slots);
        slot_table->slots = new_slot_table.slots;
        kfree(slot_table->slots_bmp);
        slot_table->slots_bmp = new_slot_table.slots_bmp;
        kfree(slot_table->full_slots_bmp);
        slot_table->full_slots_bmp = new_slot_table.full_slots_bmp;
        return 0;
}

/* should only be called when table_guard is held */
int alloc_slot_id(struct cap_group *cap_group)
{
        int empty_idx = 0, r;
        struct slot_table *slot_table;
        int bmp_size = 0, full_bmp_size = 0;

        slot_table = &cap_group->slot_table;

        while (true) {
                bmp_size = slot_table->slots_size;
                full_bmp_size = BITS_TO_LONGS(bmp_size);

                empty_idx = find_next_zero_bit(
                        slot_table->full_slots_bmp, full_bmp_size, 0);
                if (empty_idx >= full_bmp_size)
                        goto expand;

                empty_idx = find_next_zero_bit(slot_table->slots_bmp,
                                               bmp_size,
                                               empty_idx * BITS_PER_LONG);
                if (empty_idx >= bmp_size)
                        goto expand;
                else
                        break;
        expand:
                r = expand_slot_table(slot_table);
                if (r < 0)
                        goto out_fail;
        }
        BUG_ON(empty_idx < 0 || empty_idx >= bmp_size);

        set_bit(empty_idx, slot_table->slots_bmp);
        if (slot_table->slots_bmp[empty_idx / BITS_PER_LONG]
            == ~((unsigned long)0))
                set_bit(empty_idx / BITS_PER_LONG, slot_table->full_slots_bmp);

        return empty_idx;
out_fail:
        return r;
}

void *get_opaque(struct cap_group *cap_group, int slot_id, bool type_valid,
                 int type)
{
        struct slot_table *slot_table = &cap_group->slot_table;
        struct object_slot *slot;
        void *obj;

        read_lock(&slot_table->table_guard);
        if (!is_valid_slot_id(slot_table, slot_id)) {
                obj = NULL;
                goto out_unlock_table;
        }

        slot = get_slot(cap_group, slot_id);
        BUG_ON(slot->isvalid == false);
        BUG_ON(slot->object == NULL);

        if (!type_valid || slot->object->type == type) {
                obj = slot->object->opaque;
        } else {
                obj = NULL;
                goto out_unlock_table;
        }

        atomic_fetch_add_64(&slot->object->refcount, 1);

out_unlock_table:
        read_unlock(&slot_table->table_guard);
        return obj;
}

/* Get an object reference through its cap.
 * The interface will also add the object's refcnt by one.
 */
void *obj_get(struct cap_group *cap_group, int slot_id, int type)
{
        return get_opaque(cap_group, slot_id, true, type);
}

/* This is a pair interface of obj_get.
 * Used when no releasing an object reference.
 * The interface will minus the object's refcnt by one.
 *
 * Furthermore, free an object when its reference cnt becomes 0.
 */
void obj_put(void *obj)
{
        struct object *object;
        u64 old_refcount;

        object = container_of(obj, struct object, opaque);
        old_refcount = atomic_fetch_sub_64(&object->refcount, 1);

        if (old_refcount == 1) {
                extern void __free_object(struct object *);
                __free_object(object);
        }
}

int sys_create_cap_group(u64 badge, u64 cap_group_name, u64 name_len, u64 pcid)
{
        struct cap_group *new_cap_group;
        struct vmspace *vmspace;
        int cap, r;

        if ((current_cap_group->badge != ROOT_CAP_GROUP_BADGE)
            && (current_cap_group->badge != FSM_BADGE)
            && (current_cap_group->badge != PROCMGR_BADGE)) {
                kinfo("An unthorized process tries to create cap_group.\n");
                return -EPERM;
        }

        /* cap current cap_group */
        new_cap_group = obj_alloc(TYPE_CAP_GROUP, sizeof(*new_cap_group));
        if (!new_cap_group) {
                r = -ENOMEM;
                goto out_fail;
        }
        cap_group_init(new_cap_group, BASE_OBJECT_NUM, badge);

        cap = cap_alloc(current_cap_group, new_cap_group, 0);
        if (cap < 0) {
                r = cap;
                goto out_free_obj_new_grp;
        }

        /* 1st cap is cap_group */
        if (cap_copy(current_thread->cap_group, new_cap_group, cap)
            != CAP_GROUP_OBJ_ID) {
                printk("init cap_group cap[0] is not cap_group\n");
                r = -1;
                goto out_free_cap_grp_current;
        }

        /* 2st cap is vmspace */
        vmspace = obj_alloc(TYPE_VMSPACE, sizeof(*vmspace));
        if (!vmspace) {
                r = -ENOMEM;
                goto out_free_obj_vmspace;
        }

        vmspace->pcid = pcid;
        vmspace_init(vmspace);

        r = cap_alloc(new_cap_group, vmspace, 0);
        if (r < 0)
                goto out_free_obj_vmspace;
        else if (r != VMSPACE_OBJ_ID)
                BUG("init cap_group cap[1] is not vmspace\n");

        new_cap_group->notify_recycler = 0;

        /* Set the cap_group_name (process_name) for easing debugging */
        memset(new_cap_group->cap_group_name, 0, MAX_GROUP_NAME_LEN);
        if (name_len > MAX_GROUP_NAME_LEN)
                name_len = MAX_GROUP_NAME_LEN;
        copy_from_user(new_cap_group->cap_group_name,
                       (char *)cap_group_name,
                       name_len);
        return cap;
out_free_obj_vmspace:
        obj_free(vmspace);
out_free_cap_grp_current:
        cap_free(current_cap_group, cap);
        new_cap_group = NULL;
out_free_obj_new_grp:
        obj_free(new_cap_group);
out_fail:
        return r;
}

/* This is for creating the first (init) user process. */
struct cap_group *create_root_cap_group(char *name, size_t name_len)
{
        struct cap_group *cap_group;
        struct vmspace *vmspace;
        int slot_id;

        cap_group = obj_alloc(TYPE_CAP_GROUP, sizeof(*cap_group));
        BUG_ON(!cap_group);
        cap_group_init(cap_group,
                       BASE_OBJECT_NUM,
                       /* Fixed badge */ ROOT_CAP_GROUP_BADGE);

        slot_id = cap_alloc(cap_group, cap_group, 0);
        BUG_ON(slot_id != CAP_GROUP_OBJ_ID);

        vmspace = obj_alloc(TYPE_VMSPACE, sizeof(*vmspace));
        BUG_ON(!vmspace);

        /* fixed PCID 1 for root process, PCID 0 is not used. */
        vmspace->pcid = ROOT_PROCESS_PCID;
        vmspace_init(vmspace);

        slot_id = cap_alloc(cap_group, vmspace, 0);
        BUG_ON(slot_id != VMSPACE_OBJ_ID);

        /* Set the cap_group_name (process_name) for easing debugging */
        memset(cap_group->cap_group_name, 0, MAX_GROUP_NAME_LEN);
        if (name_len > MAX_GROUP_NAME_LEN)
                name_len = MAX_GROUP_NAME_LEN;
        memcpy(cap_group->cap_group_name, name, name_len);

        root_cap_group = cap_group;
        return cap_group;
}

struct clone_cap_group_args {
        u64 child_badge;
        u64 child_pcid;
        u64 child_mt_cap;
        u64 fs_server_cap;
        u64 lwip_server_cap;
        u64 procmgr_server_cap;
        u64 parent_badge;
};

extern void arch_vmspace_init(struct vmspace *);
/*
 * Create a new cap group. Clone the original vmspace for the new
 * cap group. Clone the currently running thread as the main thread
 * of the new cap group.
 * Notice: Only a normal user thread is allowed to be cloned. Kernel
 * objects like IPC connection, notification are lost. Connections to
 * system servers are rebuild in userspace.
 * Currently this function is used to implement fork().
 */
int sys_clone_cap_group(u64 clone_cap_group_args)
{
        struct cap_group *new_cap_group;
        struct vmspace *child_vmspace;
        struct clone_cap_group_args args = {0};
        struct thread *thread;
        int r;

        /* Only a normal user thread is allowed to be cloned */
        BUG_ON(current_thread->thread_ctx->type != TYPE_USER);

        /* Get args from user space */
        r = copy_from_user(
                (char *)&args, (char *)clone_cap_group_args, sizeof(args));
        BUG_ON(r);

        /* 1. Create a new cap group */
        new_cap_group = obj_alloc(TYPE_CAP_GROUP, sizeof(*new_cap_group));
        if (!new_cap_group) {
                r = -ENOMEM;
                goto out_fail;
        }
        cap_group_init(new_cap_group, BASE_OBJECT_NUM, args.child_badge);
        /* 1st cap is cap_group */
        if ((r = cap_alloc(new_cap_group, new_cap_group, 0)) < 0)
                goto out_free_cap_grp_current;
        BUG_ON(r != CAP_GROUP_OBJ_ID);
        new_cap_group->notify_recycler = 0;
        /* Set the cap_group_name to be the same as the parent process */
        memcpy(new_cap_group->cap_group_name,
               current_cap_group->cap_group_name,
               MAX_GROUP_NAME_LEN);

        /* 2. Create a new vmspace for the new cap group */
        child_vmspace = obj_alloc(TYPE_VMSPACE, sizeof(*child_vmspace));
        if (!child_vmspace) {
                r = -ENOMEM;
                goto out_free_obj_vmspace;
        }
        /* 2nd cap is vmspace */
        if ((r = cap_alloc(new_cap_group, child_vmspace, 0)) < 0)
                goto out_free_obj_vmspace;
        BUG_ON(r != VMSPACE_OBJ_ID);
        child_vmspace->pcid = args.child_pcid;
        vmspace_init(child_vmspace);

        /* 3. Create a new main thread for the new cap group */
        thread = obj_alloc(TYPE_THREAD, sizeof(*thread));
        if (!thread) {
                goto out_free_obj_thread;
        }
        if ((r = cap_alloc(new_cap_group, thread, 0)) < 0)
                goto out_free_obj_thread;

        /*
         * 4. Copy system servers' cap to new cap group and return the main
         * thread cap back to user space. The child process need these servers'
         * cap values to reinitialize connections to these system servers. It
         * also needs the main thread cap so it can pass the main thread cap to
         * procmgr to finish fork(). So we copy these values back to user space.
         * Notice: This step **must** happens before step 5. Otherwise the child
         * process cannot see the updated values.
         */
        args.fs_server_cap =
                cap_copy(current_cap_group, new_cap_group, args.fs_server_cap);
        args.lwip_server_cap = cap_copy(
                current_cap_group, new_cap_group, args.lwip_server_cap);
        args.procmgr_server_cap = cap_copy(
                current_cap_group, new_cap_group, args.procmgr_server_cap);
        args.child_mt_cap = r;
        args.parent_badge = current_cap_group->badge;
        r = copy_to_user(
                (char *)clone_cap_group_args, (char *)&args, sizeof(args));
        BUG_ON(r);

        /* 5. Clone the original vmspace */
        r = vmspace_clone(
                child_vmspace, current_thread->vmspace, new_cap_group);
        if (r < 0) {
                /* Deinitialize the child vmspace if error happens in
                 * vmspace_clone(). */
                obj_get(new_cap_group, VMSPACE_OBJ_ID, TYPE_VMSPACE);
                goto out_deinit_child_vmspace;
        }
        extern void flush_tlb_of_vmspace(struct vmspace *);
        flush_tlb_of_vmspace(current_thread->vmspace);
        /* 6. Clone current thread as the main thread for the new cap group */
        thread_clone(new_cap_group, thread);

        /* Just return some non_zero value */
        return args.child_pcid;

out_deinit_child_vmspace:
        obj_put(child_vmspace);
out_free_obj_thread:
        obj_free(thread);
out_free_obj_vmspace:
        obj_free(child_vmspace);
out_free_cap_grp_current:
        obj_free(new_cap_group);
out_fail:
        return r;
}
