#pragma once

#include <ipc/notification.h>
#include <object/object.h>

struct irq_notification {
	/* MOK: notifc should be the first field */
	struct notification notifc;
	u32 intr_vector;
	u32 status;

	/*
	 * Debugging field: Using this field to avoid re-entry of
	 * a user-level interrupt handler thread.
	 */
	volatile u32 user_handler_ready;
};

int user_handle_irq(int irq);
void irq_deinit(void *irq_ptr);

/* Syscalls */
int sys_irq_register(int irq);
int sys_irq_wait(int irq_cap, bool is_block);
int sys_irq_ack(int irq_cap);
