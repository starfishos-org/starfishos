#include <object/recycle.h>
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
#include <irq/irq.h>
#include <lib/ring_buffer.h>
#include <sched/context.h>
#ifdef CHCORE_SLS
#include <ckpt/ckpt.h>
#include <ckpt/ckpt_data.h>
#endif /* CHCORE_SLS */

extern int trans_uva_to_kva(u64 user_va, u64 *kernel_va);

struct recycle_msg {
        u64 badge;
        int exitcode;
        int padding;
};

struct recycle_msg_node {
        struct list_head node;
        struct recycle_msg msg;
        int ckpt_refcnt;
};

struct notification *recycle_notification = NULL;
struct ring_buffer *recycle_msg_buffer;
/* This list is only used when the recycle_msg_buffer is full */
struct list_head recycle_msg_head;
struct lock recycle_buffer_lock;
u64 msg_list_size;

int sys_register_recycle(int notifc_cap, vaddr_t msg_buffer)
{
        int ret;

        if (current_cap_group->badge != PROCMGR_BADGE) {
                kinfo("A process (not the procmgr) tries to register recycle.\n");
                return -EPERM;
        }

        recycle_notification =
                obj_get(current_cap_group, notifc_cap, TYPE_NOTIFICATION);

        BUG_ON(recycle_notification == NULL);

        ret = trans_uva_to_kva(msg_buffer, (vaddr_t *)&recycle_msg_buffer);
        BUG_ON(ret != 0);

        init_list_head(&recycle_msg_head);
        lock_init(&recycle_buffer_lock);
        msg_list_size = 0;

        return 0;
}

/*
 * Kernel uses this function to invoke the recycle thread in procmgr.
 * proc_badge is the badge of the process to recycle.
 */
void notify_user_recycler(u64 proc_badge, int exitcode)
{
        /* lock the recyle buffer first */
        lock(&recycle_buffer_lock);

        if (if_buffer_full(recycle_msg_buffer)) {
                /* Save the msg in the list for now */
                struct recycle_msg_node *msg;

                msg = kmalloc(sizeof(*msg), __DEFAULT__);
                msg->msg.badge = proc_badge;
                msg->msg.exitcode = exitcode;
                list_add(&msg->node, &recycle_msg_head);
                msg_list_size++;
        } else {
                struct recycle_msg tmp;
                tmp.badge = proc_badge;
                tmp.exitcode = exitcode;
                set_one_msg(recycle_msg_buffer, &tmp);

                /* Put the msg saved in list into the buffer */
                struct recycle_msg_node *msg;

                for_each_in_list (
                        msg, struct recycle_msg_node, node, &recycle_msg_head) {
                        if (!if_buffer_full(recycle_msg_buffer)) {
                                list_del(&msg->node);
                                msg_list_size--;
                                struct recycle_msg tmp2;
                                tmp2.badge = msg->msg.badge;
                                tmp2.exitcode = msg->msg.exitcode;
                                set_one_msg(recycle_msg_buffer, &tmp2);
                                kfree(msg);
                        } else {
                                break;
                        }
                }

                /* Nofity the recycle thread through the notification */
                /*
                 * The recycle thread's queue lock will only be
                 * grabbed by the kernel (in the following signal_notific)
                 * and the recycle thread itself.
                 * So, try_lock(queue_lock) in signal_notific should not fail.
                 */
                int ret;
                ret = signal_notific(recycle_notification);
                BUG_ON(ret != 0);
        }

        unlock(&recycle_buffer_lock);
}

/* All the threads in current_cap_group should exit */
void sys_exit_group(int exitcode)
{
        struct thread *thread;

        kdebug("%s\n", __func__);

#ifdef CKPT_CAP_GROUP_LAZY_COPY
        cap_group_lazy_copy_ckpt(current_cap_group);
#endif
        /*
         * Check if the notification has been sent.
         * E.g., a faulting process may trigger sys_exit_group for many times.
         */
        if (__sync_val_compare_and_swap(
                    &current_cap_group->notify_recycler, 0, 1)
            == 0) {
                /*
                 * Grap the threads_lock and set the threads state.
                 * After that, no new thread will be allocated.
                 * see `create_thread` in thread.c
                 */
#ifdef TRACK_TIME
                printk("<track> cap group [%s]:\n",
                       current_cap_group->cap_group_name);
#endif
                lock(&current_cap_group->threads_lock);
                for_each_in_list (thread,
                                  struct thread,
                                  node,
                                  &(current_cap_group->thread_list)) {
                        /* CAS is used in case the state is set to TE_EXITED
                         * concurrently */
                        __sync_val_compare_and_swap(
                                &thread->thread_ctx->thread_exit_state,
                                TE_RUNNING,
                                TE_EXITING);
#ifdef TRACK_TIME
                        printk("<track> thread: kernel %d ms, user %d ms\n",
                               current_thread->track_time_kernel / 1000000,
                               current_thread->track_time_user / 1000000);
#endif
                }
                unlock(&current_cap_group->threads_lock);

                notify_user_recycler(current_cap_group->badge, exitcode);
        }

        /* Set the exit state of current_thread: no contention */
        current_thread->thread_ctx->thread_exit_state = TE_EXITING;
        sched();
        eret_to_thread(switch_context());
}

static void recycle_server_shadow_thread(struct ipc_connection *conn,
                                         struct thread *server_thread)
{
        struct ipc_server_handler_config *config;

        config = (struct ipc_server_handler_config *)
                         server_thread->general_ipc_config;
        if (config->ipc_exit_routine_entry) {
                BUG_ON(server_thread->thread_ctx->sc);
                /*
                 * FIXME: The server shadow thread needs
                 * a temporary schuduling context to execute
                 * its exit routine. Use a more elegant way
                 * to avoid kmalloc here.
                 */
                server_thread->thread_ctx->sc =
                        kmalloc(sizeof(*server_thread->thread_ctx->sc), __DEFAULT__);
                arch_set_thread_next_ip(server_thread,
                                        config->ipc_exit_routine_entry);
                arch_set_thread_stack(server_thread, config->ipc_routine_stack);
                server_thread->thread_ctx->state = TS_INTER;
                vmspace_unmap_range(conn->server_handler_thread->vmspace,
                                    conn->shm.server_shm_uaddr,
                                    conn->shm.shm_size);
                BUG_ON(sched_enqueue(server_thread));
        }
}

/* Wait onging IPCs to finish and stop new IPCs. Also, recycle the connection
 * cap. */
// TODO: is that OK to remove the checking during IPC calls
static void stop_ipcs(struct cap_group *cap_group, struct object_slot *slot,
                      int slot_id, int *ret)
{
        struct ipc_connection *conn;
        struct thread *server_thread;

        conn = (struct ipc_connection *)slot->object->opaque;
#ifdef CKPT_CONNECTION_LAZY_COPY
        connection_lazy_copy_ckpt(conn);
#endif
        /*
         * Lock *conn*:
         * If succeed, we can then free the *conn* cap in cap_group
         * and we will not find it next time (if try again).
         * If failed, try next time.
         *
         */
        if (try_lock(&conn->ownership) != 0) {
                *ret = -EAGAIN;
                return;
        }

        /*
         * Mark the connection as invalid.
         * After, the connection will never be used. See sys_ipc_call.
         */
        conn->is_valid = INVALID;

        /*
         * If the connection is created by cap_group (to recycle),
         * then free the connection cap in the server cap_group (maybe
         * the cap_group).
         *
         */
        if (conn->client_badge == cap_group->badge) {
                server_thread = conn->server_handler_thread;
                if (server_thread) {
                        /* If the server_thread (and its cap_group) still exits.
                         *
                         * TODO: In A -> B -> C,
                         * When A exits, cap_free only removes the cap in B.
                         * But, the cap maybe passed from B to C.
                         */
                        recycle_server_shadow_thread(conn, server_thread);
                        cap_free(server_thread->cap_group,
                                 conn->conn_cap_in_server);
                        cap_free(server_thread->cap_group,
                                 conn->shm.shm_cap_in_server);
                }
        } else {
                /* cap_group is the server side of the connection */
                conn->server_handler_thread = NULL;
                /* Since we need to lock the connection again when
                 * the connection owner (client cap_group) is recycled,
                 * unlock here.
                 * Don't worry: the connection will not be used any more.
                 */
                unlock(&conn->ownership);
        }

        /* Free the connection cap (i.e., slot_id) in cap_group */
        __cap_free(cap_group, slot_id, true, false);

        /* An connection will be freed automatically when the client
         * cap_group creating it exits.
         */
}

/* Wait onging IPC registration to finish and stop newly coming ones */
static void stop_ipc_registration(struct cap_group *cap_group,
                                  struct object_slot *slot, int *ret)
{
        struct thread *thread;
        struct ipc_server_register_cb_config *config;

        thread = (struct thread *)slot->object->opaque;
        if (thread->cap_group != cap_group)
                return;

        if (thread->thread_ctx->type != TYPE_REGISTER)
                return;

        /* Avoid deadlock during try again */
        if (thread->thread_ctx->thread_exit_state == TE_EXITED)
                return;

        config = (struct ipc_server_register_cb_config *)
                         thread->general_ipc_config;
        if (try_lock(&config->register_lock) != 0) {
                /* Lock fails: registration is ongoing. So, try next time.*/
                *ret = -EAGAIN;
                return;
        }

        /*
         * No release the register_lock. So, the register_cb_thread will never
         * execute any more.
         */
        thread->thread_ctx->thread_exit_state = TE_EXITED;
        thread->thread_ctx->state = TS_EXIT;
}

static void stop_notification(struct object_slot *slot)
{
        struct notification *notific;

        notific = (struct notification *)slot->object->opaque;
        lock(&notific->notifc_lock);
        notific->state = NOTIFIC_INVALID;
        unlock(&notific->notifc_lock);
}

/*
 * FIXME:
 * Suppose cap_group is exiting, what if a thread in another cap_group transfer
 * cap to it.
 */

/*
 * Convention: sys_exit_group is executed before to notify the recycle thread
 * which then executes sys_cap_group_recycle.
 *
 * If a thread invoke this to recycle the resources, the kernel will run
 * on the thread's kernel stack, which makes things complex.
 * So, only the user-level recycler in the process manager
 * can invoke cap_group_exit on some cap_group.
 *
 * Case-1: a thread invokes exit, it will directly tell the process manager,
 *         and then, the process manager invokes this function.
 * Case-2: if a thread triggers faults (e.g., segfault), the kernel will notify
 *         the process manager to exit the corresponding process (cap_group).
 */
int sys_cap_group_recycle(int cap_group_cap)
{
        struct cap_group *cap_group;
        struct thread *thread;
        int ret;
        struct slot_table *slot_table;
        int slot_id;
        struct vmspace *vmspace = NULL;

        cap_group = obj_get(current_cap_group, cap_group_cap, TYPE_CAP_GROUP);
        if (!cap_group) {
                BUG_ON("Process Manager gives a invalid cap_group_cap.\n");
        }

#ifdef CKPT_CAP_GROUP_LAZY_COPY
        cap_group_lazy_copy_ckpt(cap_group);
#endif

        ret = 0;
        /* Phase-1: Stop all the threads in this cap_group */

        /* IPC recycle begin */
        slot_table = &cap_group->slot_table;
        write_lock(&slot_table->table_guard);

        /* Handle all the connection caps and the register_cb_thread caps */
        for_each_set_bit (
                slot_id, slot_table->slots_bmp, slot_table->slots_size) {
                struct object_slot *slot;

                slot = get_slot(cap_group, slot_id);

                if (slot->object->type == TYPE_CONNECTION) {
                        stop_ipcs(cap_group, slot, slot_id, &ret);
                } else if (slot->object->type == TYPE_THREAD) {
                        stop_ipc_registration(cap_group, slot, &ret);
                } else if (slot->object->type == TYPE_NOTIFICATION) {
                        stop_notification(slot);
                }
        }

        write_unlock(&slot_table->table_guard);
        /* IPC recycle end */

        if (ret == -EAGAIN) {
                kdebug("%s: Line: %d\n", __func__, __LINE__);
                goto out;
        }

        /*
         * As `sys_exit_group` is executed before:
         * - no new thread will be created
         * - each thread is set as TE_EXITING in that function
         */
        for_each_in_list (
                thread, struct thread, node, &(cap_group->thread_list)) {
                /* If some thread is not TE_EXITED, then return -EAGAIN. */
                if (thread->thread_ctx->thread_exit_state != TE_EXITED) {
                        /*
                         * As all the connection are set to INVALID in previous
                         * step, all the shadow threads (IPC server threads)
                         * will not execute any more.
                         * Thus, we directly set them to exited here.
                         */
                        if (thread->thread_ctx->type == TYPE_SHADOW) {
                                thread->thread_ctx->thread_exit_state =
                                        TE_EXITED;
                                continue;
                        }

                        /* FIXME: use notification in sleep @lwt */
                        if (thread->thread_ctx->state == TS_WAITING) {
                                extern void try_remove_timeout(struct thread *);
                                try_remove_timeout(thread);
                                thread->thread_ctx->thread_exit_state =
                                        TE_EXITED;
                                continue;
                        }

                        ret = -EAGAIN;
                }
        }

        if (ret == -EAGAIN) {
                kdebug("%s: Line: %d\n", __func__, __LINE__);
                goto out;
        }

        /* All the thread are TE_EXITED now, wait until their kernel stacks are
         * free */
        for_each_in_list (
                thread, struct thread, node, &(cap_group->thread_list)) {
                wait_for_kernel_stack(thread);
                BUG_ON(thread->thread_ctx->thread_exit_state != TE_EXITED);
                if (thread->thread_ctx->state != TS_EXIT)
                        kwarn("%s thread ctx->state is %d\n",
                              thread->cap_group->cap_group_name,
                              thread->thread_ctx->state);
        }

        /*
         * Phase-2:
         * Iterate all the capability table and free the corresponding
         * resources.
         */

        slot_table = &cap_group->slot_table;
        write_lock(&slot_table->table_guard);

        for_each_set_bit (
                slot_id, slot_table->slots_bmp, slot_table->slots_size) {
                struct object_slot *slot;
                struct object *object;

                slot = get_slot(cap_group, slot_id);
                BUG_ON(!slot || slot->isvalid == false);
                object = slot->object;

                if (slot_id == VMSPACE_OBJ_ID) {
                        vmspace = (struct vmspace *)(object->opaque);
                        extern void flush_tlb_of_vmspace(struct vmspace *);
                        flush_tlb_of_vmspace(vmspace);
                }

                /*
                 * TODO: according to the cap type, cap_free_all-like-procedure
                 * should be used.
                 */
                if ((object->type == TYPE_THREAD)
                    && (((struct thread *)(object->opaque))->cap_group
                        == cap_group)) {
                        /*
                         * Use cap_free_all to free the threads belong to
                         * the exited cap_group.
                         */
                        kdebug("recycle one local thread.\n");

                        /*
                         * Like cap_free_all, but without locks.
                         * Directly using cap_free_all leads to dead lock.
                         */
                        struct object_slot *slot_iter = NULL,
                                           *slot_iter_tmp = NULL;
                        // int r;

                        /* Not using obj_get or get_opaque is also for avoid
                         * deadlock. */
                        atomic_fetch_add_64(&object->refcount, 1);

                        /* free all copied slots */
                        lock(&object->copies_lock);
                        for_each_in_list_safe (slot_iter,
                                               slot_iter_tmp,
                                               copies,
                                               &object->copies_head) {
                                u64 iter_slot_id = slot_iter->slot_id;
                                struct cap_group *iter_cap_group =
                                        slot_iter->cap_group;

                                // r =
                                __cap_free(iter_cap_group,
                                           iter_slot_id,
                                           true,
                                           true);
                                // BUG_ON(r != 0);
                        }
                        unlock(&object->copies_lock);

                        obj_put(object->opaque);
                } else {
                        __cap_free(cap_group, slot_id, true, false);
                }
        }
        write_unlock(&slot_table->table_guard);

        /* The cap_group will be freed in the following cap_free_all. */
        obj_put(cap_group);
        cap_free_all(current_cap_group, cap_group_cap);

        BUG_ON(vmspace == NULL);
        kdebug("%s is done\n", __func__);

        return ret;
out:
        obj_put(cap_group);
        return ret;
}

#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
/* recycle ckpt and restore */
extern struct ckpt_obj_root *ckpt_obj_root_get(struct object *obj, int alloc);
extern struct object *restore_obj_get(struct ckpt_obj_root *ckpt_obj_root);

int recycle_create_ckpt(struct ckpt_recycle_data *recycle_data)
{
        /* ckpt recycle_msg_buffer */
        void *free_ptr;
        if (recycle_data->recycle_msg_buffer) {
                free_ptr = recycle_data->recycle_msg_buffer;
                recycle_data->recycle_msg_buffer = NULL;
                kfree(free_ptr);
        }
        if (recycle_data->recycle_msg_array) {
                free_ptr = recycle_data->recycle_msg_array;
                recycle_data->recycle_msg_array = NULL;
                kfree(free_ptr);
        }

        recycle_data->recycle_msg_buffer = ckpt_ring_buffer(recycle_msg_buffer);

        /* ckpt recycle_msg_array */
        #if defined CHCORE_SLS
        struct recycle_msg_node **recycle_msg_array =
                kmalloc(msg_list_size * sizeof(struct recycle_msg_node *), __DEFAULT__);
        #else
        struct recycle_msg_node **recycle_msg_array =
                kmalloc(msg_list_size * sizeof(struct recycle_msg_node *), __SHARED__);
        #endif
        struct recycle_msg_node *msg;
        int i = 0;
        for_each_in_list (
                msg, struct recycle_msg_node, node, &recycle_msg_head) {
                recycle_msg_array[i] = msg;
                i++;
        }

        recycle_data->recycle_msg_array = recycle_msg_array;
        recycle_data->recycle_msg_size = msg_list_size;

        BUG_ON(!recycle_notification);
        recycle_data->recycle_notification_obj_root = ckpt_obj_root_get(
                container_of(recycle_notification, struct object, opaque),
                false);
        return 0;
}

int recycle_restore(struct ckpt_recycle_data *recycle_data, struct kvs *obj_map)
{
        /* restore recycle_msg_buffer */
        recycle_msg_buffer =
                restore_ring_buffer(recycle_data->recycle_msg_buffer);

        /* restore recycle_msg_array */
        init_list_head(&recycle_msg_head);
        msg_list_size = recycle_data->recycle_msg_size;
        lock_init(&recycle_buffer_lock);
        struct recycle_msg_node **recycle_msg_array =
                recycle_data->recycle_msg_array;
        struct recycle_msg_node *msg;
        for (int i = 0; i < msg_list_size; i++) {
                msg = recycle_msg_array[i];
                list_add(&msg->node, &recycle_msg_head);
        }

        recycle_notification =
                (struct notification *)restore_obj_get(
                        recycle_data->recycle_notification_obj_root)
                        ->opaque;
        BUG_ON(!recycle_notification);
        return 0;
}
#endif /* CHCORE_SLS */
