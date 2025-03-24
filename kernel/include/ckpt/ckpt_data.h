#pragma once

#include <common/types.h>
#include <sched/sched.h>
#include <arch/sched/arch_sched.h>
#include <object/cap_group.h>
#include <ipc/connection.h>
#include <ipc/notification.h>
#include <mm/vmspace.h>
#include <common/kvstore.h>
// #define COPY_PGTBL
// #define CHECK_PCTBL

#define OBJ_OVERWRITE 1

#ifdef RESTORE_REPORT
u64 eval_restore_obj_time[TYPE_NR];
u64 eval_restore_obj_count[TYPE_NR];
#endif

struct ckpt_thread_ctx {
    arch_exec_ctx_t ec;
    /* TLS Related States */
    u64 tls_base_reg[TLS_REG_NUM];
    /* Thread Type */
    u32 type;
    /* Thread state */
    u32 state;
    /* Priority */
    u32 prio;
    /* SMP Affinity */
    s32 affinity;
    /* Thread kernel stack state */
    u32 kernel_stack_state;
    u32 thread_exit_state;
    /* TODO: add fpu */
    /* FPU States */
    void *fpu_state;
    /* Is FPU owner on some CPU: -1 means No; other means CPU ID */
    // int is_fpu_owner;
};

struct ckpt_vmregion {
    vaddr_t start;
    size_t size;
    vmr_prop_t perm;
    struct ckpt_obj_root *pmo_root;
};

struct ckpt_vmspace {
    struct ckpt_vmregion *ckpt_vmrs;
    int vmr_count;
    /* Root page table */
#ifdef COPY_PGTBL
    vaddr_t *pgtbl;
#endif
    u64 pcid;
    /* Heap-related: only used for user processes */
    int heap_vmr_idx;

    /* For the virtual address of mmap */
    vaddr_t user_current_mmap_addr;
};

struct ckpt_sleep_state {
    /* Time to wake up */
    u64 wakeup_tick;
    /* The cpu id where the thread is sleeping */
    u64 sleep_cpu;
    /*
     * Currently 2 type of callbacks: notification and sleep.
     * If it is NULL, the thread is not waiting for timeout.
     */
    timer_cb cb;
};

struct ckpt_thread {
    struct ckpt_thread_ctx thread_ctx;
    // struct list_head	notification_queue_node;

    struct ckpt_obj_root *cap_group_root;

    struct ckpt_obj_root *vmspace_root;

    // /* TODO: add sleep state */
    /*
     * Only exists for threads in a server process.
     * If not NULL, it points to one of the three config types.
     */
    void *general_ipc_config;
    struct ckpt_sleep_state sleep_state;
};

struct ckpt_object_slot {
    u64 slot_id;
    /* TODO: extern rights to a more general per-cap data storage of an object
     */
    /* now we don't use rights */
    // u64 rights;
    struct ckpt_obj_root *obj_root;
};

struct ckpt_page {
    vaddr_t va;
    u64 version_number;

    struct ckpt_page_pair *tt_page;
};
struct ckpt_page_pair {
    u8 type;
    u64 refcnt;
    struct ckpt_page pages[2];
};

enum ckpt_page_pair_type {
    CKPT_PP_DRAM,
    CKPT_PP_NVM,
    CKPT_PP_TT,
};

struct ckpt_pmobject {
    struct lock lock;
    struct radix *radix;
    paddr_t start;
    size_t size;
    pmo_type_t type;
    void *private;
    u64 flags;
#ifdef PMO_CHECKSUM
    u64 checksum;
    struct radix *radix_backup;
    u64 backup_checksum;
#endif
};

struct ckpt_cap_group {
    /* The number of threads */
    // int thread_cnt;

    /*
     * Each process has a unique badge as a global identifier which
     * is set by the system server, procmgr.
     * Currently, badge is used as a client ID during IPC.
     */
    u64 badge;

    /* Ensures the cap_group_exit function only be executed onece */
    int notify_recycler;

    /* Now is used for debugging */
    char cap_group_name[MAX_GROUP_NAME_LEN + 1];

    /* ckpt_slot_table */
    unsigned int table_size;
    unsigned int slots_size;
    struct ckpt_object_slot *slots;
};

struct ckpt_notification {
    u32 not_delivered_notifc_count;
    u32 waiting_threads_count;
    // struct list_head waiting_threads;
    /*
     * notifc_lock protects counter and list of waiting threads,
     * including the internal states of waiting threads.
     */
    // struct lock notifc_lock
    struct ckpt_obj_root **waiting_thread_roots;
    /* For recycling */
    int state;
};

struct ckpt_ipc_connection {
    /*
     * current client who uses this connection.
     * Note that all threads in the client process can use this connection.
     */
    struct ckpt_obj_root *current_client_thread_root;

    /*
     * server_handler_thread is always fixed after establishing the
     * connection.
     * i.e., ipc_server_handler_thread
     */
    struct ckpt_obj_root *server_handler_thread_root;

    /*
     * Identification of the client (cap_group).
     * This badge is always fixed with the ipc_connection and
     * will be transferred to the server during each IPC.
     * Thus, the server can identify different client processes.
     */
	badge_t client_badge;
	int client_pid;

    /* XXX: for temporary use of return cap from server to client */
    struct ipc_msg *user_ipc_msg;

    struct shm_for_ipc_connection shm;
    /* For resource recycle */
    struct lock ownership;
    cap_t conn_cap_in_client;
    cap_t conn_cap_in_server;
    int state;
};

struct ckpt_irq_notification {
    u32 intr_vector;
    u32 status;
    struct ckpt_notification notifc;
    /*
     * Debugging field: Using this field to avoid re-entry of
     * a user-level interrupt handler thread.
     */
    volatile u32 user_handler_ready;
};

struct ckpt_ring_buffer {
    size_t buffer_size;
    off_t consumer_offset;
    off_t producer_offset;
    size_t msg_size;
    vaddr_t ring_buffer_va;
};

struct ckpt_recycle_data {
    struct ckpt_obj_root *recycle_notification_obj_root;
    /*
     * buffer layout
     * 0-8: consumer_offset
     * 9-16: producer_offset
     *
     * size: valid recycle buffer size
     */
    struct ckpt_ring_buffer *recycle_msg_buffer;
    struct recycle_msg_node **recycle_msg_array;
    u64 recycle_msg_size;
};

struct ckpt_irq_data {
    u8 irq_handle_type[128];
    struct ckpt_obj_root *irq_notifcs_obj_root[128];
};

struct ckpt_fault_pending_thread {
    /* Use (fault_badge, fault_va) as key to find the pending thread */
    u64 fault_badge;
    vaddr_t fault_va;

    struct ckpt_obj_root *ckpt_thread_obj_root;

    struct list_head node;
};

/* TODO: use cow to optimize */
struct ckpt_fmap_fault_pool {
    u64 cap_group_badge;
    struct ckpt_obj_root *ckpt_notifc_obj_root;
    struct ckpt_ring_buffer *msg_buffer_kva;
    vaddr_t fmap_fault_pool;
    struct list_head node;
    struct list_head ckpt_fault_pending_thread_list;
};

struct ckpt_ws_data {
    /*
     * Map from ckpt object's uuid to ckpt object's node.
     */
    // struct kvs *map;
    /*
     * Pointer to the ckpt_root_cap_group_node.
     */
    struct ckpt_obj_root *ckpt_root_obj_root;

    /* TODO : ckpt irq */
    // struct ckpt_irq_data irq_data;

    struct ckpt_recycle_data recycle_data;
    struct list_head ckpt_fmap_fault_pool_list;

    u64 version_number;
};

struct ckpt_ws_table {
    /* a list to store ckpt in desc order */
    struct list_head ckpt_ws_list;
    /*
     * check whelter the id is vaild
     * key: ckpt_id(ckpt_ws_info*)
     * value: ckpt_ws_data
     */
    struct kvs *ckpt_ws_kvs;
    /*
     * a kvs to query
     * key: hash(name)
     * value: ckpt_ws_info list
     */
    struct kvs *name_kvs;
};

struct ckpt_object {
    u64 type;
    u64 opaque[];
};

struct ckpt_obj_root {
    struct object *obj;
#ifdef CHCORE_SSI_SLS
    struct object *obj_src;
    struct object *obj_dst;
#endif
#if OBJ_OVERWRITE == 1
    struct ckpt_object *ckpt_objs[2];
#endif
#ifdef CHCORE_SSI_SLS
    struct ckpt_object *cfork_ckpt_obj;
    /**
     * a flag to indicate whether the object is cross shared
     * if the object is cross shared, we do not need to allocate
     * a new object on the target machine.
     */
    bool cross_shared;
#endif
    bool cow;
    int flip_flag;
    u64 refcnt;
    bool time_traveling;
};

struct ckpt_ipc_server_handler_config {
    u64 config_type;
    /* Avoid invoking the same handler_thread concurrently */
    struct lock ipc_lock;

    /* PC */
    u64 ipc_routine_entry;
    /* SP */
    u64 ipc_routine_stack;

    /*
     * Record which connection uses this handler thread now.
     * Multiple connection can use the same handler_thread.
     */
    struct ckpt_obj_root *active_conn_root;
};

struct ckpt_ipc_server_register_cb_config {
    u64 config_type;
    struct lock register_lock;
    /* PC */
    u64 register_cb_entry;
    /* SP */
    u64 register_cb_stack;

    /* The caps for the connection currently building */
    int conn_cap_in_client;
    /* Not used now (can be exposed to server in future) */
    int conn_cap_in_server;
    int shm_cap_in_server;
};

struct ckpt_ipc_server_config {
    u64 config_type;
    /* Callback_thread for handling client registration */
    struct ckpt_obj_root *register_cb_thread_root;

    /* Record the argument from the server thread */
    u64 declared_ipc_routine_entry;
};

struct dsm_queue_node {
    struct list_head node;
    void *data;
};