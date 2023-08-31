#include <ipc/notification.h>
#include <irq/irq.h>
#include <object/irq.h>
#include <common/errno.h>
#include <sched/context.h>

// TODO: recycle IRQ CAP

struct irq_notification *irq_notifcs[MAX_IRQ_NUM];
int user_handle_irq(int irq)
{
	struct irq_notification *irq_notic;

	irq_notic = irq_notifcs[irq];
	BUG_ON(!irq_notic);

	/*
	 * If the interrupt handler thread is not ready for handling a new
	 * interrupt, we ignore a nested interrupt.
	 */
	if (!irq_notic->user_handler_ready) {
		kdebug("One interrupt (irq: %d) is ingored since the handler "
		       "thread is not ready.\n", irq);
		return 0;
	}

	/*
	 * Disable the irq before passing the current irq to the
	 * user-level handler thread.
	 */
	arch_disable_irqno(irq_notic->intr_vector);

	signal_irq_notific(irq_notic);
	/* Never returns. */

	BUG_ON(1);
	return 0;
}

void irq_deinit(void *irq_ptr)
{
	struct irq_notification *irq_notifc;
	int irq;

	irq_notifc = (struct irq_notification *)irq_ptr;
	irq = irq_notifc->intr_vector;
	irq_handle_type[irq] = HANDLE_KERNEL;
	smp_mb();
	irq_notifcs[irq] = NULL;
}

int sys_irq_register(int irq)
{
	struct irq_notification *irq_notifc = NULL;
	int irq_notifc_cap = 0;
	int ret = 0;

#if 0
	// Just leave for debugging
	int is_fpu_owner = current_thread->thread_ctx->is_fpu_owner;
	kinfo("%s is invoked, irqnum is %d, is_fpu_owner %d\n",
	      __func__, irq, is_fpu_owner);
	BUG_ON(smp_get_cpu_id() != 0);

	/* 235: a tmp mark for the interrupt handler thread */
	current_thread->thread_ctx->prio = 253;
#endif

	if (irq < 0 || irq >= MAX_IRQ_NUM)
		return -EINVAL;

	irq_notifc = obj_alloc(TYPE_IRQ, sizeof(*irq_notifc));
	if (!irq_notifc) {
		ret = -ENOMEM;
		goto out_fail;
	}
	irq_notifc->intr_vector = irq;
	init_notific(&irq_notifc->notifc);
	irq_notifc->user_handler_ready = 0;

	irq_notifc_cap = cap_alloc(current_cap_group, irq_notifc, 0);
	if (irq_notifc_cap < 0) {
		ret = irq_notifc_cap;
		goto out_free_obj;
	}

	irq_notifcs[irq] = irq_notifc;
	smp_mb();
	irq_handle_type[irq] = HANDLE_USER;

	return irq_notifc_cap;
out_free_obj:
	obj_free(irq_notifc);
out_fail:
	return ret;
}

int sys_irq_wait(int irq_cap, bool is_block)
{
	struct irq_notification *irq_notifc = NULL;
	int ret = 0;
	irq_notifc = obj_get(current_thread->cap_group, irq_cap, TYPE_IRQ);
	if (!irq_notifc) {
		ret = -ECAPBILITY;
		goto out;
	}

	/*
	 * When the interrupt handler thread calls this function,
	 * we enable the corresponding irq.
	 */
	arch_enable_irqno(irq_notifc->intr_vector);
	wait_irq_notific(irq_notifc);

	/* Never returns */
	BUG_ON(1);

out:
	return ret;
}

int sys_irq_ack(int irq_cap)
{
	struct irq_notification *irq_notifc = NULL;
	int ret = 0;
	irq_notifc = obj_get(current_thread->cap_group, irq_cap, TYPE_IRQ);
	if (!irq_notifc) {
		ret = -ECAPBILITY;
		goto out;
	}
	plat_ack_irq(irq_notifc->intr_vector);
	obj_put(irq_notifc);
out:
	return ret;
}
