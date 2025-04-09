#include <irq/ipi.h>
#include <machine.h>
#include <common/kprint.h>
#include <arch/machine/machine.h>
#include <ckpt/hot_pages_tracker.h>
#include <ckpt/hybird_mem.h>
#include <mm/nvm.h>

struct ipi_data {
    /* Grab this lock before writing to this ipi_data */
    struct lock lock;

    /* finish <- 1: the ipi_data (arguments) is handled */
    volatile u16 finish;

    /* finish <- 1: the core enter kernel mode */
    volatile u16 in_kernel;

    /* The IPI vector */
    u32 vector;

    u64 args[IPI_DATA_ARG_NUM];
};

/* IPI data shared among all the CPUs */
static struct ipi_data ipi_data[PLAT_CPU_NUM];
static struct ipi_data special_cpu_ipi_data;

/* Invoked once during the kernel boot */
void init_ipi_data(void)
{
    int i;

    for (i = 0; i < PLAT_CPU_NUM; ++i) {
        lock_init(&ipi_data[i].lock);
        ipi_data[i].finish = 1;
    }

    lock_init(&special_cpu_ipi_data.lock);
    special_cpu_ipi_data.finish = 1;
}

/*
 * Interfaces for inter-cpu communication (named IPI_transaction).
 * IPI-based message sending.
 */

/* Lock the target buffer */
void prepare_ipi_tx(u32 target_cpu)
{
    struct ipi_data *data_target, *data_self;

    data_target = &(ipi_data[target_cpu]);
    data_self = &(ipi_data[smp_get_cpu_id()]);

    /*
     * Handle IPI tx while waiting to avoid deadlock.
     *
     * Deadlock example:
     * CPU 0: in prepare_ipi_tx(), waiting for ipi_data[1]->lock;
     * CPU 1: in prepare_ipi_tx(), waiting for ipi_data[0]->lock;
     * CPU 2: in wait_finish_ipi_tx(), holding ipi_data[0]->lock,
     *        waiting for CPU 0 to finish an IPI tx;
     * CPU 3: in wait_finish_ipi_tx(), holding ipi_data[1]->lock,
     *        waiting for CPU 1 to finish an IPI tx.
     */
    while (try_lock(&data_target->lock)) {
        if (!data_self->finish) {
            handle_ipi();
        }
    }
}

/* Set argments */
void set_ipi_tx_arg(u32 target_cpu, u32 arg_index, u64 val)
{
    ipi_data[target_cpu].args[arg_index] = val;
}

/*
 * Start IPI-based transaction (tx).
 *
 * ipi_vector can be encoded into the physical interrupt (as IRQ number),
 * which can be used to implement some fast-path (simple) communication.
 *
 * Nevertheless, we support sending information with IPI.
 * So, actually, we can use one ipi_vector to distinguish different IPIs.
 */
void start_ipi_tx(u32 target_cpu, u32 ipi_vector)
{
    struct ipi_data *data;

    data = &(ipi_data[target_cpu]);

    /* Set ipi_vector */
    data->vector = ipi_vector;

    smp_mb();

    /* Mark the arguments are ready (set_ipi_tx_arg before) */
    data->finish = 0;

    smp_mb();

    /* Send physical IPI to interrupt the target CPU */
    arch_send_ipi(target_cpu, ipi_vector);
}

/* Wait and unlock */
void wait_finish_ipi_tx(u32 target_cpu)
{
    /*
     * It is possible that core-A is waiting for core-B to finish one IPI
     * while core-B is also waiting for core-A to finish one IPI.
     * So, this function will polling on the IPI data of both the target
     * core and the local core, namely data_target and data_self.
     */
    struct ipi_data *data_target, *data_self;

    data_target = &(ipi_data[target_cpu]);
    data_self = &(ipi_data[smp_get_cpu_id()]);

    /* Avoid dead lock by checking and handling ipi request while waiting */
    while (!data_target->finish) {
        if (!data_self->finish) {
            handle_ipi();
        }
    }

    unlock(&(data_target->lock));
}

/* Send IPI tx without argument */
void send_ipi(u32 target_cpu, u32 ipi_vector)
{
    prepare_ipi_tx(target_cpu);
    start_ipi_tx(target_cpu, ipi_vector);
    wait_finish_ipi_tx(target_cpu);
}

/* Receiver side interfaces */

#define ipi_data_self (&ipi_data[smp_get_cpu_id()])

/* Get argments */
u64 get_ipi_tx_arg(u32 arg_index)
{
    return ipi_data_self->args[arg_index];
}

/* Handle IPI tx */
void handle_ipi(void)
{
    struct ipi_data *data = ipi_data_self;

    /* The IPI tx may have been completed in wait_finish_ipi_tx() */
    if (data->finish) {
        return;
    }

    arch_handle_ipi(data->vector);

    data->finish = 1;
}

/* Wait finish without pre handle when waiting */
void wait_finish_in_kernel(u32 target_cpu)
{
    struct ipi_data *data_target;

    data_target = &(ipi_data[target_cpu]);

/* parallel migrate CPU side */
#if defined(PARALLEL_LOOP) && defined(HYBRID_MEM)
    struct list_head *sublist;
    /* if checkpoint has been started */
    if (CKPT_INITIALIZED) {
        while (data_target->in_kernel == 1) {
            /* FNFIXME: how can wait in kernel avoid dead lock? */
        }
        /* get the sublist from the passed arg */
        sublist = &(active_list[get_ipi_tx_arg(0)]);
        process_sub_active_list(sublist);

        data_target->in_kernel = 1;
        smp_mb();
    }
#endif

    /* Wait untill finish */
    /* now finish == 0, wait for finish == 1 */
    while (data_target->finish != 1) {
        /* FNFIXME: how can wait in kernel avoid dead lock? */
    }

    /* Reset start/finish */
    data_target->in_kernel = 0;

    unlock(&(data_target->lock));
}

/* signal_parallel_active_list_loop: signal other cpus to start looping */
void signal_parallel_active_list_loop()
{
    /* send signal to each CPU to enable parallel procedure of item */
    u32 i, cnt = 0;
    u32 cpuid = smp_get_cpu_id();

    for (i = 0; i < PLAT_CPU_NUM; ++i) {
        if (i == cpuid)
            continue;
        /* wait other cpu to get into ipi */
        set_ipi_tx_arg(i, 0, cnt++);
        ipi_data[i].in_kernel = 0;
        // printk("%s: mark_finish_ipi_tx\n", __func__);
    }
}

/* signal_parallel_active_list_loop: wait other cpus finish looping */
void wait_parallel_active_list_loop()
{
    u32 i, cnt;
    u32 cpuid = smp_get_cpu_id();

    /* wait finish == 0 */
    // printk("%s: wait finish == 0\n", __func__);
    while (1) {
        /* How many CPU is ready? */
        cnt = 0;
        for (i = 0; i < PLAT_CPU_NUM; ++i) {
            if (i == cpuid)
                continue;
            /* already get lock in prepare_ipi_tx */
            if (ipi_data[i].in_kernel == 1)
                cnt++;
        }

        /* All CPU is ready */
        if (cnt == PLAT_CPU_NUM - 1)
            break;
    }
#ifdef DYN_ADJUST
    adjust_tracker_config();
#endif
}

/* Wait for all ipi finish */
void wait_all_in_kernel(u32 except_cpu)
{
    u32 i;
    u32 cnt;

    while (1) {
        /* How many CPU is ready? */
        cnt = 0;
        for (i = 0; i < PLAT_CPU_NUM; ++i) {
            if (i == except_cpu)
                continue;
            /* already get lock in prepare_ipi_tx */
            if (ipi_data[i].in_kernel == 1)
                cnt++;
        }

        /* All CPU is ready */
        if (cnt == PLAT_CPU_NUM - 1)
            break;
    }
}

/* Check if the cpu is stop in kernel */
bool is_cpu_stop_in_kernel(u32 cpuid)
{
    /* already get lock in prepare_ipi_tx */
    return (ipi_data[cpuid].in_kernel == 1);
}

/* Mark the receiver (i.e., target_cpu) is handling the tx */
void mark_in_kernel_ipi_tx(u32 target_cpu)
{
    ipi_data[target_cpu].in_kernel = 1;
}

/* Mark the receiver (i.e., target_cpu) is handling the tx */
void mark_finish_ipi_tx(u32 target_cpu)
{
    ipi_data[target_cpu].finish = 1;
    // printk("mark finish ipi tx\n");
}

static void send_ipi_reset_sched_in_kernel(u32 target_cpu)
{
    prepare_ipi_tx(target_cpu);
    start_ipi_tx(target_cpu, IPI_RESET_SCHEDULE);
}

void sys_ipi_reset_sched_all()
{
    u32 target_cpu;
    u32 cpuid = smp_get_cpu_id();

    for (target_cpu = 0; target_cpu < PLAT_CPU_NUM; ++target_cpu) {
        // wait other cpu to get into ipi
        if (target_cpu == cpuid)
            continue;
        // wait other cpu to get into ipi
        send_ipi_reset_sched_in_kernel(target_cpu);
    }

    wait_all_in_kernel(cpuid);
}

void sys_ipi_stop_cpu(u32 cpuid)
{
    // u32 target_cpu;

    // printk("sys_ipi_stop_all\n");
    if (cpuid == smp_get_cpu_id())
        return;
    // wait other cpu to get into ipi
    prepare_ipi_tx(cpuid);
    start_ipi_tx(cpuid, IPI_WAIT_IN_KERNEL);
    // kinfo("send ipi to cpu %d\n", target_cpu);

    // wait_all_in_kernel(cpuid);
}

void sys_ipi_start_cpu(u32 cpuid)
{
    if (cpuid == smp_get_cpu_id())
        return;
    // wait other cpu to get into ipi
    mark_finish_ipi_tx(cpuid);
}
