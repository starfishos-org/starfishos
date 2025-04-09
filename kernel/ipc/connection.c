/*
 * Inter-**Process** Communication.
 *
 * connection: between a client cap_group and a server cap_group (two processes)
 * We store connection cap in a process' cap_group, so each thread in it can
 * use that connection.
 *
 * A connection (cap) can be used by any thread in the client cap_group.
 * A connection will be **only** served by one server thread while
 * one server thread may serve multiple connections.
 *
 * There is one PMO_SHM binding with one connection.
 *
 * Connection can only serve one IPC request at the same time.
 * Both user and kernel should check the "busy_state" of a connection.
 * Besides, register thread can also serve one registration request for one
 * time.
 *
 * Since a connection can only be shared by client threads in the same process,
 * a connection has only-one **badge** to identify the process.
 * During ipc_call, the kernel can set **badge** as an argument in register.
 *
 * Overview:
 * **IPC registration (control path)**
 *  - A server thread (S1) invokes **sys_register_server** with
 *    a register_cb_thread (S2)
 *
 *  - A client thread (C) invokes **sys_register_client(S1)**
 *	- invokes (switches) to S2 actually
 *  - S2 invokes **sys_ipc_register_cb_return** with a handler_thread (S3)
 *	- S3 will serve IPC requests later
 *	- switches back to C (finish the IPC registration)
 *
 * **IPC call/reply (data path)**
 *  - C invokes **sys_ipc_call**
 *	- switches to S3
 *  - S3 invokes **sys_ipc_return**
 *	- switches to C
 */

#include <ipc/connection.h>
#include <mm/kmalloc.h>
#include <mm/uaccess.h>
#include <sched/context.h>
#include <irq/irq.h>
#include <ckpt/ckpt.h>
#include <dsm/dsm-single.h>

#ifdef IPC_PERF_ENABLED
#include <arch/machine/pmu.h>
#define IPC_PERF_TIME_SIZE 10240
volatile bool ipc_perf_enabled;
volatile u64 ipc_perf_count_p2;
volatile u64 ipc_perf_count_p3;
volatile u64 ipc_perf_count_p7;
volatile u64 ipc_perf_count_p8;
u64 ipc_perf_time_p2[IPC_PERF_TIME_SIZE];
u64 ipc_perf_time_p3[IPC_PERF_TIME_SIZE];
u64 ipc_perf_time_p7[IPC_PERF_TIME_SIZE];
u64 ipc_perf_time_p8[IPC_PERF_TIME_SIZE];
#endif

/*
 * An extern declaration.
 * Here it is used for mapping ipc_shm.
 */
int map_pmo_in_current_cap_group(u64 pmo_cap, u64 addr, u64 perm);

/*
 * Overall, a server thread that declares a serivce with this interface
 * should specify @ipc_routine (the real ipc service routine entry) and
 * @register_thread_cap (another server thread for handling client
 * registration).
 */
static int register_server(struct thread *server, u64 ipc_routine,
                           u64 register_thread_cap, u64 destructor)
{
    struct ipc_server_config *config;
    struct thread *register_cb_thread;
    struct ipc_server_register_cb_config *register_cb_config;

    BUG_ON(server == NULL);
    if (server->general_ipc_config != NULL) {
        kinfo("A server thread can only invoke **register_server** once!\n");
        return -EINVAL;
    }

    /*
     * Check the passive thread in server for handling
     * client registration.
     */
    register_cb_thread =
            obj_get(current_cap_group, register_thread_cap, TYPE_THREAD);
    if (!register_cb_thread) {
        kinfo("A register_cb_thread is required.\n");
        return -EINVAL;
    }

    if (register_cb_thread->thread_ctx->type != TYPE_REGISTER) {
        kinfo("The register_cb_thread should be TYPE_REGISTER!\n");
        obj_put(register_cb_thread);
        return -EINVAL;
    }

    /*
     * TODO (tmac):
     * - directly using kmalloc is not that **microkernel/cap**
     * - free this memory when destoring the thread ...
     * - check kmalloc return value
     */
    config = kmalloc(sizeof(*config), __MT_OBJECT__);
    config->config_type = IPC_SERVER;
    /*
     * @ipc_routine will be the real ipc_routine_entry.
     * No need to validate such address because the server just
     * kill itself if the address is illegal.
     */
    config->declared_ipc_routine_entry = ipc_routine;

    /* Record the registration cb thread */
    config->register_cb_thread = register_cb_thread;

    /*
     * TODO (tmac): same as before.
     * - free this memory when destoring the thread ...
     * - check kmalloc return value
     */
    register_cb_config = kmalloc(sizeof(*register_cb_config), __MT_OBJECT__);
    register_cb_config->config_type = IPC_SERVER_REGISTER_CB;
    register_cb_thread->general_ipc_config = register_cb_config;

    /*
     * This lock will be used to prevent concurrent client threads
     * from registering.
     * In other words, a register_cb_thread can only serve
     * registration requests one-by-one.
     */
    lock_init(&register_cb_config->register_lock);

    /*
     * Record it (PC) as well as the thread's initial stack (SP).
     */
    register_cb_config->register_cb_entry =
            arch_get_thread_next_ip(register_cb_thread);
    register_cb_config->register_cb_stack =
            arch_get_thread_stack(register_cb_thread);
    register_cb_config->destructor = destructor;
    obj_put(register_cb_thread);

    /*
     * The last step: fill the general_ipc_config.
     * This field is also treated as the whether the server thread
     * declares an IPC service (or makes the service ready).
     */
    server->general_ipc_config = config;

    return 0;
}

void connection_deinit(void *conn)
{
    /* For now, no de-initialization is required */
}

/* Just used for storing the results of function client_connection */
struct client_connection_result {
    int client_conn_cap;
    int server_conn_cap;
    int server_shm_cap;
    struct ipc_connection *conn;
};

static int get_pmo_size(int pmo_cap)
{
    struct pmobject *pmo;
    int size;

    pmo = obj_get(current_cap_group, pmo_cap, TYPE_PMO);
    BUG_ON(!pmo);

    size = pmo->size;
    obj_put(pmo);

    return size;
}

/*
 * The function will create an IPC connection and initialize the client side
 * information. (used in sys_register_client)
 *
 * The server (register_cb_thread) will initialize the server side information
 * later (in sys_ipc_register_cb_return).
 */
static int create_connection(struct thread *client, struct thread *server,
                             int shm_cap_client, u64 shm_addr_client,
                             struct client_connection_result *res)
{
    int shm_cap_server;
    struct ipc_connection *conn;
    int ret = 0;
    int conn_cap = 0, server_conn_cap = 0;

    BUG_ON((client == NULL) || (server == NULL));

    /*
     * Copy the shm_cap to the server.
     *
     * It is reasonable to count the shared memory usage on the client.
     * So, a client should prepare the shm and tell the server.
     */
    shm_cap_server =
            cap_copy(current_cap_group, server->cap_group, shm_cap_client);

    /* Create struct ipc_connection */
    conn = obj_alloc(TYPE_CONNECTION, sizeof(*conn), __MT_OBJECT__);
    if (!conn) {
        ret = -ENOMEM;
        goto out_fail;
    }

    /* Initialize the connection (begin).
     *
     * Note that now client is applying to build the connection
     * instead of issuing an IPC.
     */
    conn->state = CONN_INCOME_STOPPED;
    conn->current_client_thread = client;
    /*
     * The register_cb_thread in server will assign the
     * server_handler_thread later.
     */
    conn->server_handler_thread = NULL;
    /*
     * The badge is now generated by the process who creates the client
     * thread. Usually, the process is the procmgr user-space service.
     * The badge needs to be unique.
     *
     * Before a process exits, it needs to close the connection with
     * servers. Otherwise, a later process may pretend to be it
     * because the badge is based on PID (if a PID is reused,
     * the same badge occur).
     * Or, the kernel should notify the server to close the
     * connections when some client exits.
     */
    conn->client_badge = current_cap_group->badge;
    conn->shm.client_shm_uaddr = shm_addr_client;
    conn->shm.shm_size = get_pmo_size(shm_cap_client);
    conn->shm.shm_cap_in_client = shm_cap_client;
    conn->shm.shm_cap_in_server = shm_cap_server;
    lock_init(&conn->ownership);

    /* Initialize the connection (end) */

    /* After initializing the object,
     * give the ipc_connection (cap) to the client.
     */
    conn_cap = cap_alloc(current_cap_group, conn, 0);
    if (conn_cap < 0) {
        ret = conn_cap;
        goto out_free_obj;
    }

    /* Give the ipc_connection (cap) to the server */
    server_conn_cap = cap_copy(current_cap_group, server->cap_group, conn_cap);
    if (server_conn_cap < 0) {
        ret = server_conn_cap;
        goto out_free_obj;
    }

    /* Preapre the return results */
    res->client_conn_cap = conn_cap;
    res->server_conn_cap = server_conn_cap;
    res->server_shm_cap = shm_cap_server;
    res->conn = conn;

    return 0;

out_free_obj:
    kinfo("%s fails out_free_obj\n", __func__);
    obj_free(conn);
out_fail:
    kinfo("%s fails out_fail\n", __func__);
    return ret;
}

/*
 * Grap the ipc lock before doing any modifications including
 * modifing the conn or sending the caps.
 */
static inline int grab_ipc_lock(struct ipc_connection *conn)
{
    struct thread *target;
    struct ipc_server_handler_config *handler_config;

    target = conn->server_handler_thread;
    handler_config =
            (struct ipc_server_handler_config *)target->general_ipc_config;

    if (strstr(current_thread_name, "test_file") != 0 && CUR_MACHINE_ID == 1) {
        kinfo("handler_config %p, lock: %p\n", 
            handler_config, &handler_config->ipc_lock);
    }

    /*
     * Grabing the ipc_lock can ensure:
     * First, avoid invoking the same handler thread.
     * Second, also avoid using the same connection.
     *
     * perf in Qemu: lock & unlock (without contention) just takes
     * about 20 cycles on x86_64.
     */

    /* Use try-lock, otherwise deadlock may happen
     * deadlock: T1: ipc-call -> Server -> resched to T2: ipc-call
     *
     * Although lock is added in user-ipc-lib, a buggy app may dos
     * the kernel.
     */

    if (try_lock(&handler_config->ipc_lock) != 0)
        return -EIPCRETRY;

    return 0;
}

static inline int release_ipc_lock(struct ipc_connection *conn)
{
    struct thread *target;
    struct ipc_server_handler_config *handler_config;

    target = conn->server_handler_thread;
    handler_config =
            (struct ipc_server_handler_config *)target->general_ipc_config;

    unlock(&handler_config->ipc_lock);

    return 0;
}

/* During an IPC: directly transfer the control flow from client to server */
static void thread_migrate_to_server(struct ipc_connection *conn, u64 arg)
{
    struct thread *target;
    struct ipc_server_handler_config *handler_config;

    target = conn->server_handler_thread;
    /* TODO: should we check whether the target is runnable here? */
    handler_config =
            (struct ipc_server_handler_config *)target->general_ipc_config;

    /*
     * Note that a server ipc handler thread can be assigned to multiple
     * connections.
     * So, it is necessary to record which connection is active.
     */
    handler_config->active_conn = conn;

    /*
     * Note that multiple client threads may share a same connection.
     * So, it is necessary to record which client thread is active.
     * Then, the server can transfer the control back to it after finishing
     * the IPC.
     */
    conn->current_client_thread = current_thread;

    /* Mark current_thread as TS_WAITING_IPC */
    current_thread->thread_ctx->state = TS_WAITING_IPC;

    /* Pass the scheduling context */
    target->thread_ctx->sc = current_thread->thread_ctx->sc;

    /* Set target thread SP/IP/arg */
#if defined(CHCORE_ARCH_X86_64)
    /* Remove the simple function calls can save about 20 cycles */
    arch_exec_ctx_t *ec;
    ec = &(target->thread_ctx->ec);
    ec->reg[RSP] = handler_config->ipc_routine_stack;
    ec->reg[RIP] = handler_config->ipc_routine_entry;
    ec->reg[RDI] = arg;
    ec->reg[RSI] = conn->client_badge;
#else
    arch_set_thread_stack(target, handler_config->ipc_routine_stack);
    arch_set_thread_next_ip(target, handler_config->ipc_routine_entry);
    /* First argument: ipc_msg */
    arch_set_thread_arg0(target, arg);
    /* Second argument: client_badge */
    arch_set_thread_arg1(target, conn->client_badge);
#endif

#ifdef IPC_PERF_ENABLED
    extern volatile bool ipc_perf_enabled;
    if (ipc_perf_enabled) {        
        extern volatile u64 ipc_perf_count_p3;
        extern u64 ipc_perf_time_p3[10240];
        extern u64 rdtsc(void);
        ipc_perf_time_p3[ipc_perf_count_p3++] = rdtsc();
    }
#endif

    /* Switch to the target thread */
    sched_to_thread(target);

    /* Function never return */
    BUG_ON(1);
}

/* During an IPC: directly transfer the control flow from server back to client
 */
static void thread_migrate_to_client(struct thread *client, u64 ret_value)
{
    /* Set return value for the target thread */
    arch_set_thread_return(client, ret_value);

    /* Switch to the client thread */
#ifdef IPC_PERF_ENABLED
    extern volatile bool ipc_perf_enabled;
    if (ipc_perf_enabled) {        
        extern volatile u64 ipc_perf_count_p7;
        extern u64 ipc_perf_time_p7[10240];
        extern u64 rdtsc(void);
        ipc_perf_time_p7[ipc_perf_count_p7++] = rdtsc();
    }
#endif
    sched_to_thread(client);

    /* Function never return */
    BUG_ON(1);
}

struct client_shm_config {
    int shm_cap;
    u64 shm_addr;
};

/* IPC related system calls */

int sys_register_server(unsigned long ipc_routine,
                        cap_t register_thread_cap,
                        unsigned long destructor)
{
    return register_server(
        current_thread, ipc_routine, register_thread_cap, destructor);
}

static cap_t sys_register_client_helper(struct thread *client, struct thread *server, u64 shm_config_ptr, bool trans_machine) 
{
    /*
     * No need to initialize actually.
     * However, fbinfer will complain without zeroing because
     * it cannot tell copy_from_user.
     */
    struct client_shm_config shm_config = {0};
    int r;
    struct client_connection_result res;

    struct ipc_server_config *server_config;
    struct thread *register_cb_thread;
    struct ipc_server_register_cb_config *register_cb_config;

    if (!server) {
        r = -ECAPBILITY;
        goto out_fail;
    }

    server_config = (struct ipc_server_config *)(server->general_ipc_config);
    if (!server_config) {
        r = -EIPCRETRY;
        goto out_fail;
    }

    /*
     * Locate the register_cb_thread first.
     * And later, directly transfer the control flow to it
     * for finishing the registration.
     *
     * The whole registration procedure:
     * client thread -> server register_cb_thread -> client thread
     */
    register_cb_thread = server_config->register_cb_thread;
    register_cb_config = (struct ipc_server_register_cb_config
                                  *)(register_cb_thread->general_ipc_config);

    /* Acquiring register_lock: avoid concurrent client registration.
     *
     * Use try_lock instead of lock since the unlock operation is done by
     * another thread and ChCore does not support mutex.
     * Otherwise, dead lock may happen.
     */
    if (try_lock(&register_cb_config->register_lock) != 0) {
        r = -EIPCRETRY;
        goto out_fail;
    }

    /* copy_from_user/copy_to_user will validate the user addresses */
    r = copy_from_user(
            (char *)&shm_config, (char *)shm_config_ptr, sizeof(shm_config));
    BUG_ON(r != 0);

    /* Map the pmo of the shared memory */
    r = map_pmo_in_current_cap_group(
            shm_config.shm_cap, shm_config.shm_addr, VMR_READ | VMR_WRITE);
    if (r != 0) {
        goto out_fail_unlock;
    }

    /* Create the ipc_connection object */
    r = create_connection(
            client, server, shm_config.shm_cap, shm_config.shm_addr, &res);
    if (r != 0) {
        goto out_fail_unlock;
    }

    res.conn->trans_machine = trans_machine;

    /* Record the connection cap of the client process */
    register_cb_config->conn_cap_in_client = res.client_conn_cap;
    register_cb_config->conn_cap_in_server = res.server_conn_cap;
    /* Record the server_shm_cap for current connection */
    register_cb_config->shm_cap_in_server = res.server_shm_cap;

    /* Mark current_thread as TS_WAITING_IPC */
    current_thread->thread_ctx->state = TS_WAITING_IPC;

    /* Set target thread SP/IP/arg */
    arch_set_thread_stack(register_cb_thread,
                          register_cb_config->register_cb_stack);
    arch_set_thread_next_ip(register_cb_thread,
                            register_cb_config->register_cb_entry);
    arch_set_thread_arg0(register_cb_thread,
                         server_config->declared_ipc_routine_entry);
    if (!trans_machine)
        obj_put(server);

    /* Pass the scheduling context */
    register_cb_thread->thread_ctx->sc = current_thread->thread_ctx->sc;

    kdebug("In %s: go to regsiter_cb_thread\n", __func__);
    /* On success: switch to the cb_thread of server  */
    sched_to_thread(register_cb_thread);

    /* Never return */
    BUG_ON(1);

out_fail_unlock:
    unlock(&register_cb_config->register_lock);
out_fail: /* Maybe EAGAIN */
    kdebug("%s failed\n", __func__);

    if (server && !trans_machine)
        obj_put(server);
    return r;
}

cap_t sys_register_client(cap_t server_cap, u64 shm_config_ptr)
{
    struct thread *client;
    struct thread *server;

    client = current_thread;

    server = obj_get(current_cap_group, server_cap, TYPE_THREAD);

    return sys_register_client_helper(client, server ,shm_config_ptr, false);
}

cap_t sys_register_fs_client(mid_t target_machine_id, u64 shm_config_ptr)
{
    struct thread *client;
    struct thread *server;

    client = current_thread;

    server = dsm_meta->tmpfs_thread[target_machine_id];

    return sys_register_client_helper(client, server, shm_config_ptr, true);
}

cap_t sys_register_fs_server(cap_t fs_cap)
{
    struct thread *tmpfs_thread = obj_get(current_cap_group, fs_cap, TYPE_THREAD);
    dsm_meta->tmpfs_thread[CUR_MACHINE_ID] = tmpfs_thread;
    tmpfs_thread->machine_id = CUR_MACHINE_ID;
    obj_put(tmpfs_thread);
    return 0;
}

// TODO (tmac): why 8?
#define MAX_CAP_TRANSFER 8
static int ipc_send_cap(struct ipc_connection *conn, struct ipc_msg *ipc_msg,
                        u64 cap_num)
{
    int i, r;
    u64 cap_slots_offset;
    u64 *cap_buf;

    if (likely(cap_num == 0)) {
        r = 0;
        goto out;
    } else if (cap_num >= MAX_CAP_TRANSFER) {
        r = -EINVAL;
        goto out;
    }

    r = copy_from_user((char *)&cap_slots_offset,
                       (char *)&ipc_msg->cap_slots_offset,
                       sizeof(cap_slots_offset));
    if (r < 0)
        goto out;

    cap_buf = kmalloc(cap_num * sizeof(*cap_buf), __MT_DEFAULT__);
    if (!cap_buf) {
        r = -ENOMEM;
        goto out;
    }

    r = copy_from_user((char *)cap_buf,
                       (char *)ipc_msg + cap_slots_offset,
                       sizeof(*cap_buf) * cap_num);
    if (r < 0)
        goto out;

    for (i = 0; i < cap_num; i++) {
        u64 dest_cap;

        kdebug("[IPC] send cap:%d\n", cap_buf[i]);
        dest_cap = cap_copy(current_cap_group,
                            conn->server_handler_thread->cap_group,
                            cap_buf[i]);
        if (dest_cap < 0)
            goto out_free_cap;
        cap_buf[i] = dest_cap;
    }
    /* FIXME: Copied cap id of the dest cap_group is readable from src
     * cap_group. */
    r = copy_to_user((char *)ipc_msg + cap_slots_offset,
                     (char *)cap_buf,
                     sizeof(*cap_buf) * cap_num);
    if (r < 0)
        goto out_free_cap;
    kfree(cap_buf);
    return 0;
out_free_cap:
    for (--i; i >= 0; i--)
        cap_free(conn->server_handler_thread->cap_group, cap_buf[i]);
    kfree(cap_buf);
out:
    return r;
}

/* FIXME: for temporary use of return cap from server to client */
static void ipc_send_cap_to_client(struct ipc_connection *conn, u64 cap_num)
{
    int r, i;
    u64 ret_cap;
    struct ipc_msg *server_ipc_msg;

    if (likely(cap_num == 0)) {
        return;
    } else if (cap_num >= MAX_CAP_TRANSFER) {
        return;
    }

    server_ipc_msg = (struct ipc_msg *)((u64)conn->user_ipc_msg
                                        - conn->shm.client_shm_uaddr
                                        + conn->shm.server_shm_uaddr);

    for (i = 0; i < cap_num; ++i) {
        r = copy_from_user((char *)&ret_cap,
                           (char *)server_ipc_msg
                                   + server_ipc_msg->cap_slots_offset
                                   + sizeof(ret_cap) * i,
                           sizeof(ret_cap));
        BUG_ON(r < 0);

        ret_cap = cap_copy(current_cap_group,
                           conn->current_client_thread->cap_group,
                           ret_cap);
        BUG_ON(ret_cap < 0);
        r = copy_to_user((char *)server_ipc_msg
                                 + server_ipc_msg->cap_slots_offset
                                 + sizeof(ret_cap) * i,
                         (char *)&ret_cap,
                         sizeof(ret_cap));
        BUG_ON(r < 0);
    }
}

/* Issue an IPC request */
u64 sys_ipc_call(u32 conn_cap, struct ipc_msg *ipc_msg_in_client, u64 cap_num)
{
    // TODO: 
#ifdef IPC_PERF_ENABLED
    extern volatile bool ipc_perf_enabled;
    if (ipc_perf_enabled) {        
        extern volatile u64 ipc_perf_count_p2;
        extern u64 ipc_perf_time_p2[10240];
        extern u64 rdtsc(void);
        ipc_perf_time_p2[ipc_perf_count_p2++] = rdtsc();
    }
#endif
    struct ipc_connection *conn;
    u64 ipc_msg_in_server;
    int r = 0;

    /*
     * TODO:
     * Perf on Qemu: obj_get & obj_put takes about 160 cycles
     * on x86_64.
     *
     * Use the following instead:
     * conn = (struct ipc_connection *)
     * (current_cap_group->slot_table.slots[conn_cap]->object->opaque);
     */

    conn = obj_get(current_cap_group, conn_cap, TYPE_CONNECTION);
    if (unlikely(!conn)) {
        kinfo("%s: an invalid conn_cap %lx is passed. current cap group=%s\n",
              __func__,
              conn_cap,
              current_cap_group->cap_group_name);
        return -ECAPBILITY;
    }

#ifdef CKPT_CONNECTION_LAZY_COPY
    connection_lazy_copy_ckpt(conn);
#endif
    /* For recycling:
     * Try to acquire the lock of *conn*
     *     -> Succeed: check if server_handler_thread is NULL
     *         -> Yes: return error
     *         -> No: continue normal path
     *     -> Fail: check if current_thread (client thread) is TE_EXITING
     *         -> Yes: redo Step-1
     *         -> No: return and try next time (other threads may hold the lock)
     */
    if (try_lock(&conn->ownership) == 0) {
        /*
         * Succeed in locking.
         *
         * If the connection is INVALID (setted in sys_ipc_return or
         * sys_recycle_cap_group),
         * then returns an ERROR to the invoker.
         */
        if (conn->state != CONN_VALID) {
            unlock(&conn->ownership);
            obj_put(conn);
            return -ESRCH;
        }
    } else {
        /* Fails to lock the connection */
        obj_put(conn);

        if (current_thread->thread_ctx->thread_exit_state == TE_EXITING) {
            /* The connection is locked by the recycler */

            if (current_thread->thread_ctx->type == TYPE_SHADOW) {
                /*
                 * The current thread is B in chained IPC
                 * (A:B:C). B will receives an Error.
                 * We hope B invokes sys_ipc_return to give
                 * the control flow back to A and unlock the
                 * related connection.
                 *
                 * FIXME: B may do not return the control flow
                 * back to A. If so, A will always hang and the
                 * recycler (sys_recycle_cap_group) cannot lock
                 * the connection. Extra mechanism like timeout
                 * is required.
                 */
                return -ESRCH;
            } else {
                /* Current thread will be set to exited by
                 * the scheduler */
                sched();
                eret_to_thread(switch_context());
            }
        } else {
            /* The connection is locked by someone else */
            return -EIPCRETRY;
        }
    }

    /* try_lock may fail and returns egain.
     * No modifications happen before locking, so the client
     * can simply try again later.
     */
    if ((r = grab_ipc_lock(conn)) != 0)
        goto out_obj_put;

    /* Perf: Fast Path */
    if (ipc_msg_in_client == NULL) {
        thread_migrate_to_server(conn, 0);
        BUG("should not reach here\n");
    }

    /*
     * **ipc_send_cap** only reads server_handler_thread in conn,
     * so it is not necessary to grab the lock here.
     */
    if (unlikely(cap_num != 0)) {
        r = ipc_send_cap(conn, ipc_msg_in_client, cap_num);
        if (r < 0)
            goto out_obj_put2;
    }

    conn->user_ipc_msg = ipc_msg_in_client;

    /*
     * A shm is bound to one connection.
     * But, the client and server can map the shm at different addresses.
     * So, we re-calculate the ipc_msg (in the shm) address here.
     */
    ipc_msg_in_server = (u64)ipc_msg_in_client - conn->shm.client_shm_uaddr
                        + conn->shm.server_shm_uaddr;
    /* Call server (handler thread) */
    thread_migrate_to_server(conn, ipc_msg_in_server);

    BUG("should not reach here\n");

out_obj_put2:
    release_ipc_lock(conn);
out_obj_put:
    unlock(&conn->ownership);
    obj_put(conn);
    return r;
}

int sys_ipc_return(u64 ret, u64 cap_num)
{
    struct ipc_server_handler_config *handler_config;
    struct ipc_connection *conn;
    struct thread *client;

    /* Get the currently active connection */
    handler_config = (struct ipc_server_handler_config *)
                        current_thread->general_ipc_config;
    conn = handler_config->active_conn;
    if (!conn)
            return -EINVAL;

#ifdef CKPT_CONNECTION_LAZY_COPY
    connection_lazy_copy_ckpt(conn);
#endif

    /*
     * Get the client thread that issues this IPC.
     *
     * Note that it is **unnecessary** to set the field to NULL
     * i.e., conn->current_client_thread = NULL.
     */
    client = conn->current_client_thread;

    /* Step-1. check if current_thread (conn->server_handler_thread) is
     * TE_EXITING
     *     -> Yes: set server_handler_thread to NULL, then continue to Step-2
     *     -> No: continue to Step-2
     */
    if (current_thread->thread_ctx->thread_exit_state == TE_EXITING) {
        kdebug("%s:%d Step-1\n", __func__, __LINE__);

        conn->state = CONN_INCOME_STOPPED;
        
        current_thread->thread_ctx->state = TS_EXIT;
        kinfo("%s: thread %s exit\n", current_thread->cap_group->cap_group_name, __func__);
        current_thread->thread_ctx->thread_exit_state = TE_EXITED;

        /* Returns an error to the client */
        ret = -ESRCH;
    }

    /* Step-2. check if client_thread is TS_EXITING
     *     -> Yes: set current_client_thread to NULL
     *	  Then check if client is shadow
     *         -> No: set client to TS_EXIT and then sched
     *         -> Yes: return to client (it will recycle itself at next
     *ipc_return)
     *     -> No: return normally
     */
    if (client->thread_ctx->thread_exit_state == TE_EXITING) {
        kdebug("%s:%d Step-2\n", __func__, __LINE__);

        /*
         * Currently, a connection is assumed to belong to the client process.
         * So, it the client is exiting, then the connection is useless.
         */

        conn->state = CONN_INCOME_STOPPED;

        /* If client thread is not TYPE_SHADOW, then directly mark it as
         * exited and reschedule.
         *
         * Otherwise, client thread is B in a chained IPC (A:B:C) and
         * current_thread is C. So, C returns to B and later B will
         * returns to A.
         */
        if (client->thread_ctx->type != TYPE_SHADOW) {
            kdebug("%s:%d Step-2.0\n", __func__, __LINE__);
            handler_config->active_conn = NULL;

            current_thread->thread_ctx->state = TS_WAITING_IPC;
            current_thread->thread_ctx->sc = NULL;

            unlock(&handler_config->ipc_lock);

            unlock(&conn->ownership);
            obj_put(conn);
            
            client->thread_ctx->state = TS_EXIT;
            kinfo("%s: thread %s exit\n", client->cap_group->cap_group_name, __func__);
            client->thread_ctx->thread_exit_state = TE_EXITED;

            sched();
            eret_to_thread(switch_context());
            /* The control flow will never go through */
        }
    }

    if (unlikely(cap_num != 0)) {
        ipc_send_cap_to_client(conn, cap_num);
    }

    /* Set active_conn to NULL since the IPC will finish sooner */
    handler_config->active_conn = NULL;

    /*
    * Return control flow (sched-context) back later.
    * Set current_thread state to TS_WAITING_IPC again.
    */
    current_thread->thread_ctx->state = TS_WAITING_IPC;

    /*
     * Shadow thread should not any more use
     * the client's scheduling context.
     *
     * Note that the sc of server_thread (current_thread) must be set to
     * NULL (named OP-SET-NULL) **before** unlocking the lock.
     * Otherwise, a following client thread may transfer its sc to the
     * server_thread before OP-SET-NULL.
     */

    current_thread->thread_ctx->sc = NULL;

    /*
     * Release the ipc_lock to mark the server_handler_thread can
     * serve other requests now.
     */
    unlock(&handler_config->ipc_lock);

    unlock(&conn->ownership);
    obj_put(conn);

    /* Return to client */
    thread_migrate_to_client(client, ret);
    BUG("should not reach here\n");
}

int sys_ipc_register_cb_return(u64 server_handler_thread_cap,
                                u64 server_thread_exit_routine,
                                u64 server_shm_addr)
{
    struct ipc_server_register_cb_config *config;
    struct ipc_connection *conn;
    struct thread *client_thread;

    struct thread *ipc_server_handler_thread;
    struct ipc_server_handler_config *handler_config;
    int r = 0;

    config = (struct ipc_server_register_cb_config *)
                     current_thread->general_ipc_config;
    if (!config)
        goto out_fail;

    /* Get the connection currently building */
    conn = obj_get(
            current_cap_group, config->conn_cap_in_server, TYPE_CONNECTION);
    if (!conn)
        goto out_fail;

#ifdef CKPT_CONNECTION_LAZY_COPY
    connection_lazy_copy_ckpt(conn);
#endif
    /* Get the client_thread that issues this registration */
    client_thread = conn->current_client_thread;
    /*
     * Set the return value (conn_cap) for the client here
     * because the server has approved the registration.
     */
    arch_set_thread_return(client_thread, config->conn_cap_in_client);

    /*
     * @server_handler_thread_cap from server.
     * Server uses this handler_thread to serve ipc requests.
     */
    ipc_server_handler_thread = (struct thread *)obj_get(
            current_cap_group, server_handler_thread_cap, TYPE_THREAD);
    if (!ipc_server_handler_thread)
        goto out_fail_put_conn;

    /*
     * Initialize the ipc configuration for the handler_thread (begin)
     *
     * When the handler_config isn't NULL, it means this server handler
     * thread has been initialized before. If so, skip the initialization.
     * This will happen when a server uses one server handler thread for
     * serving multiple client threads.
     */
    if (!ipc_server_handler_thread->general_ipc_config) {
        handler_config = (struct ipc_server_handler_config *)kmalloc(
                sizeof(*handler_config), __MT_DEFAULT__);
        // For ckpt
        handler_config->config_type = IPC_SERVER_HANDLER;
        handler_config->active_conn = NULL;

        ipc_server_handler_thread->general_ipc_config = handler_config;
        lock_init(&handler_config->ipc_lock);

        /*
         * Record the initial PC & SP for the handler_thread.
         * For serving each IPC, the handler_thread starts from the
         * same PC and SP.
         */
        handler_config->ipc_routine_entry =
                arch_get_thread_next_ip(ipc_server_handler_thread);
        handler_config->ipc_routine_stack =
                arch_get_thread_stack(ipc_server_handler_thread);
        handler_config->ipc_exit_routine_entry = server_thread_exit_routine;
        handler_config->destructor = config->destructor;
    }
    obj_put(ipc_server_handler_thread);
    /* Initialize the ipc configuration for the handler_thread (end) */

    /* Map the shm of the connection in server */
    r = map_pmo_in_current_cap_group(
            config->shm_cap_in_server, server_shm_addr, VMR_READ | VMR_WRITE);
    if (r != 0)
        goto out_fail_put_thread;

    /* Fill the server information in the IPC connection. */
    conn->shm.server_shm_uaddr = server_shm_addr;
    conn->server_handler_thread = ipc_server_handler_thread;
    conn->state = CONN_VALID;
    conn->current_client_thread = NULL;
    conn->conn_cap_in_client = config->conn_cap_in_client;
    conn->conn_cap_in_server = config->conn_cap_in_server;
    obj_put(conn);

    /*
     * Return control flow (sched-context) back later.
     * Set current_thread state to TS_WAITING_IPC again.
     */
    current_thread->thread_ctx->state = TS_WAITING_IPC;

    unlock(&config->register_lock);

    /* Register thread should not any more use the client's scheduling context.
     */
    current_thread->thread_ctx->sc = NULL;

    /* Finish the registration: switch to the original client_thread */
    sched_to_thread(client_thread);
    /* Nerver return */

out_fail_put_thread:
    obj_put(ipc_server_handler_thread);
out_fail_put_conn:
    obj_put(conn);
out_fail:
    return r;
}

/* Send cap through IPC */

static int transfer_cap(struct ipc_connection *conn, u32 send_cap)
{
    int cap;
    struct thread *target;

    target = conn->server_handler_thread;
    if ((cap = cap_copy(current_cap_group, target->cap_group, send_cap)) < 0)
        return cap;

    thread_migrate_to_server(conn, cap);
    return 0;
}

u64 sys_ipc_send_cap(u32 conn_cap, u32 send_cap)
{
    struct ipc_connection *conn;
    int r;

    kdebug("In %s\n", __func__);

    /* obj_put will be done in later thread_migrate_to_server */
    conn = obj_get(current_cap_group, conn_cap, TYPE_CONNECTION);
    if (!conn) {
        r = -ECAPBILITY;
        goto out_fail;
    }

    if ((r = grab_ipc_lock(conn)) != 0)
        goto out_fail;

#ifdef CKPT_CONNECTION_LAZY_COPY
    connection_lazy_copy_ckpt(conn);
#endif
    r = transfer_cap(conn, send_cap);

out_fail:
    return r;
}
