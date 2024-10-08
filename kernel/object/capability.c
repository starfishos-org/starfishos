#include <machine.h>
#include <object/object.h>
#include <object/cap_group.h>
#include <object/endpoint.h>
#include <object/thread.h>
#include <object/irq.h>
#include <mm/kmalloc.h>
#include <mm/uaccess.h>
#include <lib/printk.h>
#include <ckpt/ckpt.h>
#include <ckpt/ckpt_data.h>
#include <mm/nvm.h>

extern void pmo_deinit(void *);
extern void connection_deinit(void *);
extern void notification_deinit(void *);
extern void vmspace_deinit(void *);
extern void cap_group_deinit(void *);

const obj_deinit_func obj_deinit_tbl[TYPE_NR] = {
        [0 ... TYPE_NR - 1] = NULL,
        [TYPE_CAP_GROUP] = cap_group_deinit,
        [TYPE_THREAD] = thread_deinit,
        [TYPE_CONNECTION] = connection_deinit,
        [TYPE_NOTIFICATION] = notification_deinit,
        [TYPE_IRQ] = irq_deinit,
        [TYPE_PMO] = pmo_deinit,
        [TYPE_VMSPACE] = vmspace_deinit,
};

/*
 * Usage:
 * obj = obj_alloc(...);
 * initialize the obj;
 * cap_alloc(obj);
 */
void *obj_alloc(u64 type, u64 size, int flags)
{
        u64 total_size;
        struct object *object;

        total_size = sizeof(*object) + size;
        object = kzalloc(total_size, flags);
        if (!object)
                return NULL;

        object->type = type;
        object->size = size;
        object->refcount = 0;
        object->obj_root = NULL;

        /*
         * If the cap of the object is copied, then the copied cap (slot) is
         * stored in such a list.
         */
        init_list_head(&object->copies_head);
        lock_init(&object->copies_lock);

        return object->opaque;
}

/*
 * After the fail initialization of a cap (after obj_alloc and before
 * cap_alloc), invoke this interface to free the object allocated by obj_alloc.
 */
void obj_free(void *obj)
{
        struct object *object;

        if (!obj)
                return;
        object = container_of(obj, struct object, opaque);

        BUG_ON(object->refcount != 0);
        kfree(object);
}

int cap_alloc(struct cap_group *cap_group, void *obj, u64 rights)
{
        struct object *object;
        struct slot_table *slot_table;
        struct object_slot *slot;
        int r, slot_id;

#ifdef CKPT_CAP_GROUP_LAZY_COPY
        cap_group_lazy_copy_ckpt(cap_group);
#endif

        object = container_of(obj, struct object, opaque);
        slot_table = &cap_group->slot_table;

        write_lock(&slot_table->table_guard);
        slot_id = alloc_slot_id(cap_group);
        if (slot_id < 0) {
                r = -ENOMEM;
                goto out_unlock_table;
        }

        slot = kmalloc(sizeof(*slot), __DEFAULT__);
        if (!slot) {
                r = -ENOMEM;
                goto out_free_slot_id;
        }
        slot->slot_id = slot_id;
        slot->cap_group = cap_group;
        slot->isvalid = true;
        slot->rights = rights;
        slot->object = object;
        list_add(&slot->copies, &object->copies_head);

        BUG_ON(object->refcount != 0);
        object->refcount = 1;

        install_slot(cap_group, slot_id, slot);

        write_unlock(&slot_table->table_guard);
        return slot_id;
out_free_slot_id:
        free_slot_id(cap_group, slot_id);
out_unlock_table:
        write_unlock(&slot_table->table_guard);
        return r;
}

#ifndef TEST_OBJECT
extern struct lock fpu_owner_locks[];
/* @object->type == TYPE_THREAD */
void clear_fpu_owner(struct object *object)
{
        struct thread *thread;
        int cpuid;

        thread = (struct thread *)object->opaque;
        cpuid = thread->thread_ctx->is_fpu_owner;
        /* If is_fpu_owner >= 0, then the thread is the FPU owner of some CPU.
         */
        if (cpuid >= 0) {
                /*
                 * If the thread to free is the FPU owner of some CPU,
                 * then clear the FPU owner on that CPU first.
                 */
                lock(&fpu_owner_locks[cpuid]);
                if (cpu_info[cpuid].fpu_owner == thread)
                        cpu_info[cpuid].fpu_owner = NULL;
                unlock(&fpu_owner_locks[cpuid]);
                thread->thread_ctx->is_fpu_owner = -1;
        }
}
#endif

/* An internal interface: only invoked by __cap_free and obj_put. */
void __free_object(struct object *object)
{
#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
        extern struct ckpt_obj_root *ckpt_obj_root_get(struct object *, bool);
        struct ckpt_obj_root *root = ckpt_obj_root_get(object, false);
        if (root) {
                return;
        }
#endif /* CHCORE_SLS */
#ifndef TEST_OBJECT
        obj_deinit_func func;

        if (object->type == TYPE_THREAD)
                clear_fpu_owner(object);

        /* Invoke the object-specific free routine */
        func = obj_deinit_tbl[object->type];
        if (func)
                func(object->opaque);
#endif

        BUG_ON(!list_empty(&object->copies_head));
        kfree(object);
}

/*
 * cap_free (__cap_free) only removes one cap, which differs from cap_free_all.
 * TODO: we do not support cap_revoke for now.
 */
int __cap_free(struct cap_group *cap_group, int slot_id, bool slot_table_locked,
               bool copies_list_locked)
{
#ifdef CKPT_CAP_GROUP_LAZY_COPY
        cap_group_lazy_copy_ckpt(cap_group);
#endif
        struct object_slot *slot;
        struct object *object;
        struct slot_table *slot_table;
        int r = 0;
        u64 old_refcount;

        /* Step-1: free the slot_id (i.e., the capability number) in the slot
         * table */
        slot_table = &cap_group->slot_table;
        if (!slot_table_locked)
                write_lock(&slot_table->table_guard);
        slot = get_slot(cap_group, slot_id);
        if (!slot || slot->isvalid == false) {
                r = -ECAPBILITY;
                goto out_unlock_table;
        }

        free_slot_id(cap_group, slot_id);
        if (!slot_table_locked)
                write_unlock(&slot_table->table_guard);

        /* Step-2: remove the slot in the copies-list of the object and free the
         * slot */
        object = slot->object;
        if (copies_list_locked) {
                list_del(&slot->copies);
        } else {
                lock(&object->copies_lock);
                list_del(&slot->copies);
                unlock(&object->copies_lock);
        }
        kfree(slot);

        /* Step-3: decrease the refcnt of the object and free it if necessary */
        old_refcount = atomic_fetch_sub_64(&object->refcount, 1);

        if (old_refcount == 1)
                __free_object(object);

        return 0;

out_unlock_table:
        if (!slot_table_locked)
                write_unlock(&slot_table->table_guard);
        return r;
}

int cap_free(struct cap_group *cap_group, int slot_id)
{
        return __cap_free(cap_group, slot_id, false, false);
}

int cap_copy(struct cap_group *src_cap_group, struct cap_group *dest_cap_group,
             int src_slot_id)
{
#ifdef CKPT_CAP_GROUP_LAZY_COPY
        cap_group_lazy_copy_ckpt(dest_cap_group);
#endif
        struct object_slot *src_slot, *dest_slot;
        int r, dest_slot_id;
        struct rwlock *src_table_guard, *dest_table_guard;
        bool local_copy;

        struct object *object;

        local_copy = (src_cap_group == dest_cap_group);
        src_table_guard = &src_cap_group->slot_table.table_guard;
        dest_table_guard = &dest_cap_group->slot_table.table_guard;
        if (local_copy) {
                write_lock(dest_table_guard);
        } else {
                /* avoid deadlock */
                while (true) {
                        read_lock(src_table_guard);
                        if (write_try_lock(dest_table_guard) == 0)
                                break;
                        read_unlock(src_table_guard);
                }
        }

        src_slot = get_slot(src_cap_group, src_slot_id);
        if (!src_slot || src_slot->isvalid == false) {
                r = -ECAPBILITY;
                goto out_unlock;
        }

        dest_slot_id = alloc_slot_id(dest_cap_group);
        if (dest_slot_id == -1) {
                r = -ENOMEM;
                goto out_unlock;
        }

        dest_slot = kmalloc(sizeof(*dest_slot), __DEFAULT__);
        if (!dest_slot) {
                r = -ENOMEM;
                goto out_free_slot_id;
        }
        src_slot = get_slot(src_cap_group, src_slot_id);
        if (!src_slot || src_slot->isvalid == false) {
                r = -ECAPBILITY;
                goto out_free_slot;
        }
        atomic_fetch_add_64(&src_slot->object->refcount, 1);

        dest_slot->slot_id = dest_slot_id;
        dest_slot->cap_group = dest_cap_group;
        dest_slot->isvalid = true;
        dest_slot->object = src_slot->object;

        object = src_slot->object;
        lock(&object->copies_lock);
        list_add(&dest_slot->copies, &src_slot->copies);
        unlock(&object->copies_lock);

        install_slot(dest_cap_group, dest_slot_id, dest_slot);

        write_unlock(dest_table_guard);
        if (!local_copy)
                read_unlock(src_table_guard);
        return dest_slot_id;
out_free_slot:
        kfree(dest_slot);
out_free_slot_id:
        free_slot_id(dest_cap_group, dest_slot_id);
out_unlock:
        write_unlock(dest_table_guard);
        if (!local_copy)
                read_unlock(src_table_guard);
        return r;
}

int cap_move(struct cap_group *src_cap_group, struct cap_group *dest_cap_group,
             int src_slot_id)
{
        int r;

        r = cap_copy(src_cap_group, dest_cap_group, src_slot_id);
        if (r < 0)
                return r;

        r = cap_free(src_cap_group, src_slot_id);
        BUG_ON(r); /* if copied successfully, free should not fail */

        return r;
}

/*
 * Free an object points by some cap, which also removes all the caps point to
 * the object.
 */
int cap_free_all(struct cap_group *cap_group, int slot_id)
{
        void *obj;
        struct object *object;
        struct object_slot *slot_iter = NULL, *slot_iter_tmp = NULL;
        int r = 0;

        /*
         * Since obj_get requires to pass the cap type
         * which is not available here, get_opaque is used instead.
         */
        obj = get_opaque(cap_group, slot_id, false, 0);

        if (!obj) {
                r = -ECAPBILITY;
                goto out_fail;
        }

        object = container_of(obj, struct object, opaque);
        /* free all copied slots */
        lock(&object->copies_lock);
        for_each_in_list_safe (
                slot_iter, slot_iter_tmp, copies, &object->copies_head) {
                u64 iter_slot_id = slot_iter->slot_id;
                struct cap_group *iter_cap_group = slot_iter->cap_group;

                // r =
                __cap_free(iter_cap_group, iter_slot_id, false, true);
                // BUG_ON(r != 0);
        }
        unlock(&object->copies_lock);

        /* get_opaque will also add the reference cnt */
        obj_put(obj);

        return 0;

out_fail:
        return r;
}

int sys_cap_copy_to(u64 dest_cap_group_cap, u64 src_slot_id)
{
        struct cap_group *dest_cap_group;
        int r;

        dest_cap_group =
                obj_get(current_cap_group, dest_cap_group_cap, TYPE_CAP_GROUP);
        if (!dest_cap_group)
                return -ECAPBILITY;
        r = cap_copy(current_cap_group, dest_cap_group, src_slot_id);
        obj_put(dest_cap_group);
        return r;
}

int sys_cap_copy_from(u64 src_cap_group_cap, u64 src_slot_id)
{
        struct cap_group *src_cap_group;
        int r;

        src_cap_group =
                obj_get(current_cap_group, src_cap_group_cap, TYPE_CAP_GROUP);
        if (!src_cap_group)
                return -ECAPBILITY;
        r = cap_copy(src_cap_group, current_cap_group, src_slot_id);
        obj_put(src_cap_group);
        return r;
}

#ifndef FBINFER
int sys_transfer_caps(u64 dest_group_cap, u64 src_caps_buf, int nr_caps,
                      u64 dst_caps_buf)
{
        struct cap_group *dest_cap_group;
        int i;
        int *src_caps;
        int *dst_caps;
        size_t size;

        dest_cap_group =
                obj_get(current_cap_group, dest_group_cap, TYPE_CAP_GROUP);
        if (!dest_cap_group)
                return -ECAPBILITY;

        size = sizeof(int) * nr_caps;
        src_caps = kmalloc(size, __DEFAULT__);
        dst_caps = kmalloc(size, __DEFAULT__);

        /* get args from user buffer */
        copy_from_user((void *)src_caps, (void *)src_caps_buf, size);

        for (i = 0; i < nr_caps; ++i) {
                // TODO: check error
                dst_caps[i] = cap_copy(
                        current_cap_group, dest_cap_group, src_caps[i]);
        }

        /* write results to user buffer */
        copy_to_user((void *)dst_caps_buf, (void *)dst_caps, size);

        kfree(src_caps);
        kfree(dst_caps);

        obj_put(dest_cap_group);
        return 0;
}
#endif

int sys_cap_move(u64 dest_cap_group_cap, u64 src_slot_id)
{
        struct cap_group *dest_cap_group;
        int r;

        dest_cap_group =
                obj_get(current_cap_group, dest_cap_group_cap, TYPE_CAP_GROUP);
        if (!dest_cap_group)
                return -ECAPBILITY;
        r = cap_move(current_cap_group, dest_cap_group, src_slot_id);
        obj_put(dest_cap_group);
        return r;
}

// for debug
int sys_get_all_caps(u64 cap_group_cap)
{
        struct cap_group *cap_group;
        struct slot_table *slot_table;
        int i;

        cap_group = obj_get(current_cap_group, cap_group_cap, TYPE_CAP_GROUP);
        if (!cap_group)
                return -ECAPBILITY;
        printk("thread %p cap:\n", current_thread);

        slot_table = &cap_group->slot_table;
        for (i = 0; i < slot_table->slots_size; i++) {
                struct object_slot *slot = get_slot(cap_group, i);
                if (!slot)
                        continue;
                BUG_ON(slot->isvalid != true);
                printk("slot_id:%d type:%d\n",
                       i,
                       slot_table->slots[i]->object->type);
        }

        obj_put(cap_group);
        return 0;
}

int sys_revoke_cap(u64 obj_cap)
{
        int ret = 0;

        ret = cap_free(current_cap_group, obj_cap);
        return ret;
}

int cap_insert(struct cap_group *cap_group, void *obj, u64 rights, int slot_id)
{
        struct object *object;
        struct slot_table *slot_table;
        struct object_slot *slot;
        int r;

        object = container_of(obj, struct object, opaque);
        slot_table = &cap_group->slot_table;

        write_lock(&slot_table->table_guard);
        /* TODO: check whether slot_id is allocated */
        /* set bmp */
        BUG_ON(slot_id < 0 || slot_id >= slot_table->slots_size);
        set_bit(slot_id, slot_table->slots_bmp);
        if (slot_table->full_slots_bmp[slot_id / BITS_PER_LONG]
            == ~((unsigned long)0))
                set_bit(slot_id / BITS_PER_LONG, slot_table->full_slots_bmp);

        slot = kmalloc(sizeof(*slot), __DEFAULT__);
        if (!slot) {
                r = -ENOMEM;
                goto out_free_slot_id;
        }
        slot->slot_id = slot_id;
        slot->cap_group = cap_group;
        slot->isvalid = true;
        slot->rights = rights;
        slot->object = object;
        list_add(&slot->copies, &object->copies_head);
        // BUG_ON(object->refcount != 0);
        atomic_fetch_add_64(&object->refcount, 1);
        install_slot(cap_group, slot_id, slot);
        write_unlock(&slot_table->table_guard);
        return slot_id;
out_free_slot_id:
        free_slot_id(cap_group, slot_id);
        // out_unlock_table:
        write_unlock(&slot_table->table_guard);
        return r;
}

int cap_replace(struct cap_group *cap_group, void *obj, int slot_id)
{
        struct object *old_obj;
        struct object *object;
        struct slot_table *slot_table;
        struct object_slot *slot;

        object = container_of(obj, struct object, opaque);
        slot_table = &cap_group->slot_table;

        write_lock(&slot_table->table_guard);
        BUG_ON(slot_id < 0 || slot_id >= slot_table->slots_size);
        BUG_ON(!get_bit(slot_id, cap_group->slot_table.slots_bmp));

        slot = cap_group->slot_table.slots[slot_id];

        old_obj = slot->object;
        if (old_obj == object) {
                goto unlock;
        }

        slot->object = object;
        list_add(&slot->copies, &object->copies_head);
        object->refcount++;

unlock:
        write_unlock(&slot_table->table_guard);
        return slot_id;
}
