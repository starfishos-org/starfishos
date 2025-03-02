#ifdef CHCORE_ENABLE_FMAP

#pragma once

#include <ipc/notification.h>
#include <common/lock.h>

/* User ring buffer node */
struct user_fault_msg {
    u64 fault_badge;
    vaddr_t fault_va;
};

/**
 * Save pending thread, and enqueue them when user mapping finished
 */
struct fault_pending_thread {
    /* Use (fault_badge, fault_va) as key to find the pending thread */
    u64 fault_badge;
    vaddr_t fault_va;

    struct thread *thread;

    struct list_head node;
};

/**
 * A fmap_fault_pool is ownered by a vmspace(cap_group)
 * If thread call sys_user_fault_register,
 * we will create a fmap_fault_pool for the cap_group,
 * and add to fmap_fault_pool_list.
 */
struct fmap_fault_pool {
    u64 cap_group_badge;
    struct notification *notific;
    struct ring_buffer *msg_buffer_kva; // TODO(FN): changed field

    /* fault pending thread list */
    struct list_head pending_threads;

    struct lock lock;
    struct list_head node;
};

extern struct lock fmap_fault_pool_list_lock;
extern struct list_head fmap_fault_pool_list;

void handle_user_fault(struct pmobject *pmo, vaddr_t fault_va);

/* Syscalls */
int sys_user_fault_register(int notific_cap, vaddr_t msg_buffer);
int sys_user_fault_map(u64 client_badge, vaddr_t fault_va, vaddr_t remap_va,
                       bool copy);

int fmap_fault_pool_create_ckpt(struct list_head *);
int fmap_fault_pool_restore(struct list_head *, struct kvs *);
#endif
